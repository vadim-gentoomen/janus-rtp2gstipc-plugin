/*! \file   janus_rtp2gstipc.c
 * 
 * \author Vadim Dynnik
 * 
 * \copyright GNU General Public License v3
 * 
 * \brief  Janus RTP to Gstreamer IPC plugin
 * 
 * \details See README.md
*/

#include "janus_rtp2gstipc.h"

static janus_plugin rtp2gstipc_plugin =
	JANUS_PLUGIN_INIT (
		.init = rtp2gstipc_init,
		.destroy = rtp2gstipc_destroy,

		.get_api_compatibility = rtp2gstipc_get_api_compatibility,
		.get_version = rtp2gstipc_get_version,
		.get_version_string = rtp2gstipc_get_version_string,
		.get_description = rtp2gstipc_get_description,
		.get_name = rtp2gstipc_get_name,
		.get_author = rtp2gstipc_get_author,
		.get_package = rtp2gstipc_get_package,

		.create_session = rtp2gstipc_create_session,
		.handle_message = rtp2gstipc_handle_message,
		.setup_media = rtp2gstipc_setup_media,
		.incoming_rtp = rtp2gstipc_incoming_rtp,
		.incoming_rtcp = rtp2gstipc_incoming_rtcp,
		.incoming_data = rtp2gstipc_incoming_data,
		.slow_link = rtp2gstipc_slow_link,
		.hangup_media = rtp2gstipc_hangup_media,
		.destroy_session = rtp2gstipc_destroy_session,
		.query_session = rtp2gstipc_query_session,
	);

janus_plugin *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", RTP2GSTIPC_NAME);
	return &rtp2gstipc_plugin;
}

static volatile gint initialized = 0, stopping = 0;
static janus_callbacks *gateway = NULL;
static GThread *handler_thread;
static GThread *watchdog_thread;

static void *rtp2gstipc_handler_thread(void *data);

static GHashTable *sessions;
static janus_mutex sessions_mutex = JANUS_MUTEX_INITIALIZER;

static void rtp2gstipc_session_destroy(rtp2gstipc_session *session) {
	if(session && g_atomic_int_compare_and_exchange(&session->destroyed, 0, 1))
		janus_refcount_decrease(&session->ref);
}

static void rtp2gstipc_session_free(const janus_refcount *session_ref) {
	rtp2gstipc_session *session = janus_refcount_containerof(session_ref, rtp2gstipc_session, ref);
	/* Remove the reference to the core plugin session */
	janus_refcount_decrease(&session->handle->ref);
	/* This session can be destroyed, free all the resources */
	g_free(session);
}

static void rtp2gstipc_message_free(rtp2gstipc_message *msg) {
	if(!msg || msg == &exit_message)
		return;

	msg->handle = NULL;

	g_free(msg->transaction);
	msg->transaction = NULL;
	if(msg->body)
		json_decref(msg->body);
	msg->body = NULL;
	if(msg->jsep)
		json_decref(msg->jsep);
	msg->jsep = NULL;

	g_free(msg);
}

/* Error codes */
#define RTP2GSTIPC_ERROR_NO_MESSAGE         411
#define RTP2GSTIPC_ERROR_INVALID_JSON       412
#define RTP2GSTIPC_ERROR_INVALID_ELEMENT    413
#define RTP2GSTIPC_ERROR_INVALID_SDP        414
#define RTP2GSTIPC_ERROR_MISSING_ELEMENT    415
#define RTP2GSTIPC_ERROR_UNKNOWN_ERROR      416


int rtp2gstipc_init(janus_callbacks *callback, const char *config_path) {
    if (g_atomic_int_get(&stopping)) {
        return -1;
    }

    if (callback == NULL || config_path == NULL) {
        /* Invalid arguments */
        return -1;
    }

    sessions = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify) rtp2gstipc_session_destroy);
    messages = g_async_queue_new_full((GDestroyNotify) rtp2gstipc_message_free);
    gateway = callback;

    GError *error = NULL;

    handler_thread = g_thread_try_new("rtp2gstipc message handler thread", rtp2gstipc_handler_thread, NULL, &error);
    if (error != NULL) {
        JANUS_LOG(LOG_ERR, "%s Got error %d (%s) trying to launch the message handler thread...\n", RTP2GSTIPC_NAME,
                  error->code, error->message ? error->message : "??");
        return -1;
    }

    g_atomic_int_set(&initialized, 1);
    JANUS_LOG(LOG_INFO, "%s initialized!\n", RTP2GSTIPC_NAME);
    gst_init(NULL, NULL);
    return 0;
}


void rtp2gstipc_destroy(void) {
	JANUS_LOG(LOG_INFO, "%s destroying...\n", RTP2GSTIPC_NAME);

	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(messages, &exit_message);
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}
	if(watchdog_thread != NULL) {
		g_thread_join(watchdog_thread);
		watchdog_thread = NULL;
	}

	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	janus_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;
	sessions = NULL;

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);

	JANUS_LOG(LOG_INFO, "%s destroyed!\n", RTP2GSTIPC_NAME);
}

int rtp2gstipc_get_api_compatibility(void) {
	return JANUS_PLUGIN_API_VERSION;
}

int rtp2gstipc_get_version(void) {
	return RTP2GSTIPC_VERSION;
}

const char *rtp2gstipc_get_version_string(void) {
	return RTP2GSTIPC_VERSION_STRING;
}

const char *rtp2gstipc_get_description(void) {
	return RTP2GSTIPC_DESCRIPTION;
}

const char *rtp2gstipc_get_name(void) {
	return RTP2GSTIPC_NAME;
}

const char *rtp2gstipc_get_author(void) {
	return RTP2GSTIPC_AUTHOR;
}

const char *rtp2gstipc_get_package(void) {
	return RTP2GSTIPC_PACKAGE;
}

void rtp2gstipc_create_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}

	rtp2gstipc_session *session = (rtp2gstipc_session *)g_malloc0(sizeof(rtp2gstipc_session));
	session->handle = handle;
	janus_refcount_init(&session->ref, rtp2gstipc_session_free);

	session->sendport_video_rtp = 0;
	session->sendport_video_rtcp = 0;
	session->sendport_audio_rtp = 0;
	session->sendport_audio_rtcp = 0;

	session->sendsockfd = -1;
	session->sendsockaddr = (struct sockaddr_in){ .sin_family = AF_INET };

	strcpy(session->negotiate_acodec, "opus");
	strcpy(session->negotiate_vcodec, "vp8");

	session->video_enabled = TRUE;
	session->audio_enabled = TRUE;
	session->enable_video_on_keyframe = FALSE;
	session->disable_video_on_packetloss = FALSE;

	session->seqnr_video_last = 0;
	session->seqnr_audio_last = 0;

	session->fir_seqnr = 0;

	session->drop_permille = 0;
	session->drop_video_packets = 0;
	session->drop_audio_packets = 0;

	janus_rtp_switching_context_reset(&session->context);

	g_atomic_int_set(&session->destroyed, 0);
	g_atomic_int_set(&session->hangingup, 0);

	handle->plugin_handle = session;

	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
	janus_mutex_unlock(&sessions_mutex);

	JANUS_LOG(LOG_INFO, "%s Session created.\n", RTP2GSTIPC_NAME);
	return;
}


void rtp2gstipc_destroy_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_mutex_lock(&sessions_mutex);
	rtp2gstipc_session *session = (rtp2gstipc_session *)handle->plugin_handle;
	if(!session) {
		janus_mutex_unlock(&sessions_mutex);
		JANUS_LOG(LOG_ERR, "%s rtp2gstipc_destroy_session: No session associated with this handle...\n", RTP2GSTIPC_NAME);
		*error = -2;
		return;
	}

	JANUS_LOG(LOG_INFO, "%s Destroy session...\n", RTP2GSTIPC_NAME);
	close(session->sendsockfd);

	if(session->relay_thread != NULL) {
		JANUS_LOG(LOG_INFO, "%s Watchdog: Joining session's relay thread\n", RTP2GSTIPC_NAME);
		g_thread_join(session->relay_thread); // blocking
		session->relay_thread = NULL;
		JANUS_LOG(LOG_INFO, "%s Watchdog: Session's relay thread joined\n", RTP2GSTIPC_NAME);
	}

	g_hash_table_remove(sessions, handle);

	janus_mutex_unlock(&sessions_mutex);

	JANUS_LOG(LOG_INFO, "%s Session destroyed.\n", RTP2GSTIPC_NAME);
	return;
}

json_t *rtp2gstipc_query_session(janus_plugin_session *handle) {
	return json_object();
}



struct janus_plugin_result *rtp2gstipc_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep) {
	JANUS_LOG(LOG_INFO, "%s rtp2gstipc_handle_message.\n", RTP2GSTIPC_NAME);

	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		// Synchronous
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);


	rtp2gstipc_session *session = (rtp2gstipc_session *)handle->plugin_handle;
	int error_code = 0;
	char *error_cause = g_malloc0(512);
	json_t *response = NULL;

	json_t *enable_video_on_keyframe = json_object_get(message, "enable_video_on_keyframe");
	if (enable_video_on_keyframe) {
		session->enable_video_on_keyframe = (gboolean)json_is_true(enable_video_on_keyframe);
		JANUS_LOG(LOG_INFO, "%s session->enable_video_on_keyframe %s\n", RTP2GSTIPC_NAME, (session->enable_video_on_keyframe ? "TRUE" : "FALSE"));
	}

	json_t *disable_video_on_packetloss = json_object_get(message, "disable_video_on_packetloss");
	if (disable_video_on_packetloss) {
		session->disable_video_on_packetloss = (gboolean)json_is_true(disable_video_on_packetloss);
		JANUS_LOG(LOG_INFO, "%s session->disable_video_on_packetloss %s\n", RTP2GSTIPC_NAME, (session->disable_video_on_packetloss ? "TRUE" : "FALSE"));
	}

	json_t *drop_probability = json_object_get(message, "drop_probability");
	if (drop_probability) {
		session->drop_permille = (guint16)json_integer_value(drop_probability);
		JANUS_LOG(LOG_INFO, "%s session->drop_permille=%d\n", RTP2GSTIPC_NAME, session->drop_permille);
	}

	json_t *drop_video_packets = json_object_get(message, "drop_video_packets");
	if (drop_video_packets) {
		session->drop_video_packets = (guint16)json_integer_value(drop_video_packets);
		JANUS_LOG(LOG_INFO, "%s session->drop_video_packets=%d\n", RTP2GSTIPC_NAME, session->drop_video_packets);
	}

	json_t *drop_audio_packets = json_object_get(message, "drop_audio_packets");
	if (drop_audio_packets) {
		session->drop_audio_packets = (guint16)json_integer_value(drop_audio_packets);
		JANUS_LOG(LOG_INFO, "%s session->drop_audio_packets=%d\n", RTP2GSTIPC_NAME, session->drop_audio_packets);
	}

	json_t *video_enabled = json_object_get(message, "video_enabled");
	if (video_enabled) {
		session->video_enabled = (gboolean)json_is_true(video_enabled);
		JANUS_LOG(LOG_INFO, "%s session->video_enabled=%s\n", RTP2GSTIPC_NAME, session->video_enabled ? "TRUE" : "FALSE");
	}

	json_t *audio_enabled = json_object_get(message, "audio_enabled");
	if (audio_enabled) {
		session->audio_enabled = (gboolean)json_is_true(audio_enabled);
		JANUS_LOG(LOG_INFO, "%s session->audio_enabled=%s\n", RTP2GSTIPC_NAME, session->audio_enabled ? "TRUE" : "FALSE");
	}


	json_t *request = json_object_get(message, "request");
	if (request) {
		const char *request_text = json_string_value(request);

		if(!strcmp(request_text, "configure")) {

			const char *negotiate_acodec = json_string_value(json_object_get(message, "negotiate_acodec"));
			if (negotiate_acodec) {
				// For supported audio codecs, see sdp-utils.c
				if (!strcmp(negotiate_acodec, "pcmu")) {
					strcpy(session->negotiate_acodec, "pcmu");
				} else if (!strcmp(negotiate_acodec, "pcma")) {
					strcpy(session->negotiate_acodec, "pcma");
				} else if (!strcmp(negotiate_acodec, "g722")) {
					strcpy(session->negotiate_acodec, "g722");
				} else if (!strcmp(negotiate_acodec, "isac16")) {
					strcpy(session->negotiate_acodec, "isac16");
				} else if (!strcmp(negotiate_acodec, "isac32")) {
					strcpy(session->negotiate_acodec, "isac32");
				} else {
					// "opus" or default
					strcpy(session->negotiate_acodec, "opus");
				}
			}

			const char *negotiate_vcodec = json_string_value(json_object_get(message, "negotiate_vcodec"));
			if (negotiate_vcodec) {
				// For supported video codecs, see sdp-utils.c
				if (!strcmp(negotiate_vcodec, "h264")) {
					strcpy(session->negotiate_vcodec, "h264");
				} else if (!strcmp(negotiate_vcodec, "vp9")) {
					strcpy(session->negotiate_vcodec, "vp9");
				} else {
					// "vp8" or default
					strcpy(session->negotiate_vcodec, "vp8");
				}
			}

			guint16 sendport_video_rtp = (guint16)json_integer_value(json_object_get(message, "sendport_video_rtp"));
			if (sendport_video_rtp) {
				JANUS_LOG(LOG_INFO, "%s Will forward to port %d\n", RTP2GSTIPC_NAME, sendport_video_rtp);
				session->sendport_video_rtp = sendport_video_rtp;
			} else {
				JANUS_LOG(LOG_ERR, "%s JSON error: Missing element: sendport_video_rtp\n", RTP2GSTIPC_NAME);
				error_code = RTP2GSTIPC_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "JSON error: Missing element: sendport_video_rtp");
				goto respond;
			}

			guint16 sendport_video_rtcp = (guint16)json_integer_value(json_object_get(message, "sendport_video_rtcp"));
			if (sendport_video_rtcp) {
				JANUS_LOG(LOG_INFO, "%s Will forward to port %d\n", RTP2GSTIPC_NAME, sendport_video_rtcp);
				session->sendport_video_rtcp = sendport_video_rtcp;
			} else {
				JANUS_LOG(LOG_ERR, "%s JSON error: Missing element: sendport_video_rtcp\n", RTP2GSTIPC_NAME);
				error_code = RTP2GSTIPC_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "JSON error: Missing element: sendport_video_rtcp");
				goto respond;
			}

			guint16 sendport_audio_rtp = (guint16)json_integer_value(json_object_get(message, "sendport_audio_rtp"));
			if (sendport_audio_rtp) {
				JANUS_LOG(LOG_INFO, "%s Will forward to port %d\n", RTP2GSTIPC_NAME, sendport_audio_rtp);
				session->sendport_audio_rtp = sendport_audio_rtp;
			} else {
				JANUS_LOG(LOG_ERR, "%s JSON error: Missing element: sendport_audio_rtp\n", RTP2GSTIPC_NAME);
				error_code = RTP2GSTIPC_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "JSON error: Missing element: sendport_audio_rtp");
				goto respond;
			}

			guint16 sendport_audio_rtcp = (guint16)json_integer_value(json_object_get(message, "sendport_audio_rtcp"));
			if (sendport_audio_rtcp) {
				JANUS_LOG(LOG_INFO, "%s Will forward to port %d\n", RTP2GSTIPC_NAME, sendport_audio_rtcp);
				session->sendport_audio_rtcp = sendport_audio_rtcp;
			} else {
				JANUS_LOG(LOG_ERR, "%s JSON error: Missing element: sendport_audio_rtcp\n", RTP2GSTIPC_NAME);
				error_code = RTP2GSTIPC_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "JSON error: Missing element: sendport_audio_rtcp");
				goto respond;
			}

			const char *sendipv4 = json_string_value(json_object_get(message, "sendipv4"));
			if (sendipv4) {
				JANUS_LOG(LOG_INFO, "%s Will forward to IPv4 %s\n", RTP2GSTIPC_NAME, sendipv4);
				session->sendsockaddr.sin_addr.s_addr = inet_addr(sendipv4);
			} else {
				JANUS_LOG(LOG_ERR, "%s JSON error: Missing element: sendipv4\n", RTP2GSTIPC_NAME);
				error_code = RTP2GSTIPC_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "JSON error: Missing element: sendipv4");
				goto respond;
			}

			// close socket if already open
			if (session->sendsockfd) {
				close(session->sendsockfd);
				session->sendsockfd = -1;
			}

			// create and configure socket
			session->sendsockfd = socket(AF_INET, SOCK_DGRAM, 0);
			if (session->sendsockfd < 0) { // error
				JANUS_LOG(LOG_ERR, "%s Could not create sending socket\n", RTP2GSTIPC_NAME);
				error_code = 99; // TODO: define this
				g_snprintf(error_cause, 512, "Could not create sending socket");
				goto respond;
			}
			if (IN_MULTICAST(ntohl(inet_addr(sendipv4)))) {
				uint8_t ttl = 0; // do not route UDP packets outside of local host
				setsockopt(session->sendsockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

				struct in_addr mcast_iface_addr;
				// We explicitly choose the multicast network interface, otherwise the kernel will choose for us.
				// We go for the software loopback interface for low latency. A physical ethernet card could add latency.
				mcast_iface_addr.s_addr = htonl(INADDR_LOOPBACK);

				JANUS_LOG(LOG_WARN, "%s: This rtp2gstipc session will multicast to IP multicast address %s "
				"because you specified it. The IP_MULTICAST_TTL option has been set to 0 (zero), which "
				"SHOULD cause at least the first router (the Linux kernel) to NOT forward the UDP packets. "
				"The behavior is is however OS-specific. You SHOULD verify that the UDP packets "
				"are not inadvertenly forwarded into network zones where the security/privacy of the packets "
				"could be compromised.\n", RTP2GSTIPC_NAME, inet_ntoa(session->sendsockaddr.sin_addr));

				JANUS_LOG(LOG_WARN, "%s: Will multicast from network interface with IP %s\n", RTP2GSTIPC_NAME, inet_ntoa(mcast_iface_addr));

				setsockopt(session->sendsockfd, IPPROTO_IP, IP_MULTICAST_IF, &mcast_iface_addr, sizeof(mcast_iface_addr));
			}

			response = json_object();
			json_object_set_new(response, "configured", json_string("ok"));
			goto respond;

		} else if (!strcmp(request_text, "pli")) {
			gateway->send_pli(session->handle);
			response = json_object();
			goto respond;

		} else if (!strcmp(request_text, "fir")) {
			gateway->send_pli(session->handle);
			response = json_object();
			goto respond;


		} else if (!strcmp(request_text, "remb")) {
			uint32_t bitrate = (uint32_t)json_integer_value(json_object_get(message, "bitrate"));
			if (bitrate) {
				gateway->send_remb(session->handle, bitrate ? bitrate : 10000000);

				response = json_object();
			} else {
				JANUS_LOG(LOG_ERR, "%s JSON error: Missing element: bitrate\n", RTP2GSTIPC_NAME);
				error_code = RTP2GSTIPC_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "JSON error: Missing element: bitrate");
			}
			goto respond;

		}
	} // if 'request' key in msg


	/* async handling for all other messages.
	 * In particular, JSEP offers/answer need to be done asynchronously, because janus_plugin_push_event() in janus.c merges SDP.
	 */
	rtp2gstipc_message *msg = g_malloc0(sizeof(rtp2gstipc_message));
	msg->handle = handle;
	msg->transaction = transaction;
	msg->body = message; // guaranteed by Janus to be an object
	msg->jsep = jsep;
	g_async_queue_push(messages, msg);
	return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, "Processing asynchronously", NULL);

respond:
	{
		if(message != NULL)
			json_decref(message);
		if(jsep != NULL)
			json_decref(jsep);
		g_free(transaction);

		if(error_code == 0 && !response) {
			error_code = RTP2GSTIPC_ERROR_UNKNOWN_ERROR;
			g_snprintf(error_cause, 512, "Invalid response");
		}

		if(error_code != 0) {
			/* Prepare JSON error event */
			json_t *errevent = json_object();
			json_object_set_new(errevent, "rtp2gstipc", json_string("event"));
			json_object_set_new(errevent, "error_code", json_integer(error_code));
			json_object_set_new(errevent, "error", json_string(error_cause));
			return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, errevent);

		} else {
			return janus_plugin_result_new(JANUS_PLUGIN_OK, NULL, response);
		}
	}
}

void janus_init_gstreamer(rtp2gstipc_session *session) {
    g_print("22222\n");
    session->pipeline = GST_PIPELINE(gst_pipeline_new(NULL));
    session->v_appsrc = (GstAppSrc*)gst_element_factory_make("appsrc", NULL);
    session->a_appsrc = (GstAppSrc*)gst_element_factory_make("appsrc", NULL);
    session->v_rtcp_appsrc = (GstAppSrc*)gst_element_factory_make("appsrc", NULL);
    session->a_rtcp_appsrc = (GstAppSrc*)gst_element_factory_make("appsrc", NULL);

//    rtp2gst(session);
}

void rtp2gstipc_setup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "%s WebRTC media is now available.\n", RTP2GSTIPC_NAME);
    rtp2gstipc_session *session = (rtp2gstipc_session *)handle->plugin_handle;
    janus_init_gstreamer(session);
}

void janus_gstreamer_push_rtp_buffer(rtp2gstipc_session *session, char *buf, int len, gboolean is_video) {
    g_print("RTP buffer\n");
    guchar *rtp = (guchar *) g_malloc(len);
    memcpy(rtp, buf, len);
    if (is_video) {
        GstBuffer *v_buffer = gst_buffer_new_wrapped_full(0, rtp, len, 0, len, rtp, g_free);
        gst_app_src_push_buffer((GstAppSrc *) session->v_appsrc, v_buffer);
    } else {
        GstBuffer *a_buffer = gst_buffer_new_wrapped_full(0, rtp, len, 0, len, rtp, g_free);
        gst_app_src_push_buffer((GstAppSrc *) session->a_appsrc, a_buffer);
    }
}

void rtp2gstipc_incoming_rtp(janus_plugin_session *handle, janus_plugin_rtp *packet) {
	rtp2gstipc_session *session = (rtp2gstipc_session *)handle->plugin_handle; // simple and fast. echotest does the same.

	if (session->sendsockfd < 0) return; // not yet configured: skip if no socket open

	struct sockaddr_in addr = session->sendsockaddr;
	socklen_t addrlen = sizeof(struct sockaddr_in);

	if (session->drop_permille > g_random_int_range(0,1000))
		return; // simulate bad connection

    gboolean video = packet->video;
    char *buf = packet->buffer;
    uint16_t len = packet->length;
    if (janus_is_rtp(buf, len)) {
        janus_gstreamer_push_rtp_buffer(session, buf, len, video);
    }

    // отправка по UDP
	janus_rtp_header *header = (janus_rtp_header *)packet->buffer;
	guint16 seqn_current = ntohs(header->seq_number);

	if (packet->video) { // VIDEO

		if (session->drop_video_packets > 0) {
			session->drop_video_packets--;
			return;
		}

		guint16 seqnr_last = session->seqnr_video_last;
		guint16 missed = seqn_current - seqnr_last - (guint16)1;
		if (
			!seqnr_last || // first packet
			seqn_current < seqnr_last // guint16 has wrapped (TODO: this also ignores packet misordering)
		) {
			missed = 0;
		}

		if (missed) {
			JANUS_LOG(LOG_WARN, "%s Missed %d video packets before sequence number %d\n", RTP2GSTIPC_NAME, missed, seqn_current);

			// We have missed at least one packet.
			// Some downstream decoders could be sensitive to packet loss.
			// In this case, it is recommended to stop video forwarding, and only
			// re-start it at the next keyframe.
			if (session->disable_video_on_packetloss && session->video_enabled) {
				JANUS_LOG(LOG_WARN, "%s Disabling video forwarding because of packet loss\n", RTP2GSTIPC_NAME);
				session->video_enabled = FALSE;
			}
		}

		// Detect keyframes and maybe re-enable video.
		gboolean is_keyframe;
		int plen = 0;
		char *payload = janus_rtp_payload(packet->buffer, packet->length, &plen);
		if (session->vcodec == CODEC_VP8) {
			is_keyframe = janus_vp8_is_keyframe(payload, plen);
		} else if (session->vcodec == CODEC_VP9) {
			is_keyframe = janus_vp9_is_keyframe(payload, plen);
		} else if (session->vcodec == CODEC_H264) {
			is_keyframe = janus_h264_is_keyframe(payload, plen);
		}
		if (is_keyframe) {
			JANUS_LOG(LOG_DBG, "%s Received keyframe\n", RTP2GSTIPC_NAME);
			if (session->enable_video_on_keyframe && !session->video_enabled) {
				JANUS_LOG(LOG_WARN, "%s Enabling video forwarding because of keyframe\n", RTP2GSTIPC_NAME);
				session->video_enabled = TRUE;
			}
		}

		session->seqnr_video_last = seqn_current;

		if (!session->video_enabled)
			return;

		addr.sin_port = htons(session->sendport_video_rtp);


	} else { // AUDIO
		if (session->drop_audio_packets > 0) {
			session->drop_audio_packets--;
			return;
		}

		guint16 seqnr_last = session->seqnr_audio_last;
		guint16 missed = seqn_current - seqnr_last - (guint16)1;
		if (
			!seqnr_last || // first packet
			seqn_current < seqnr_last // guint16 has wrapped (TODO: this also ignores packet misordering)
		) {
			missed = 0;
		}

		if (missed) {
			JANUS_LOG(LOG_WARN, "%s Missed %d audio packets before sequence number %d\n", RTP2GSTIPC_NAME, missed, seqn_current);
		}

		session->seqnr_audio_last = seqn_current;

		if (!session->audio_enabled)
			return;

		addr.sin_port = htons(session->sendport_audio_rtp);
	}

	// forward to the selected UDP port
	int numsent = sendto(session->sendsockfd, packet->buffer, packet->length, 0, (struct sockaddr*)&addr, addrlen);
}

void janus_gstreamer_push_rtcp_buffer(rtp2gstipc_session *session, char *buf, int len, gboolean is_video) {
    g_print("RTCP buffer\n");
    guchar *rtcp = (guchar *) g_malloc(len);
    memcpy(rtcp, buf, len);
    if (is_video) {
        GstBuffer *v_rtcp_buffer = gst_buffer_new_wrapped_full(0, rtcp, len, 0, len, rtcp, g_free);
        gst_app_src_push_buffer((GstAppSrc *) session->v_rtcp_appsrc, v_rtcp_buffer);
    } else {
        GstBuffer *a_rtcp_buffer = gst_buffer_new_wrapped_full(0, rtcp, len, 0, len, rtcp, g_free);
        gst_app_src_push_buffer((GstAppSrc *) session->a_rtcp_appsrc, a_rtcp_buffer);
    }
}

void rtp2gstipc_incoming_rtcp(janus_plugin_session *handle, janus_plugin_rtcp *packet) {
	rtp2gstipc_session *session = (rtp2gstipc_session *)handle->plugin_handle;
	if (session->sendsockfd < 0) return;
	struct sockaddr_in addr = session->sendsockaddr;
	socklen_t addrlen = sizeof(struct sockaddr_in);

    char *buf = packet->buffer;
    uint16_t len = packet->length;
    if (janus_is_rtcp(buf, len)) {
        janus_gstreamer_push_rtcp_buffer(session, buf, len, packet->video);
    }

	if (packet->video) {
		addr.sin_port = htons(session->sendport_video_rtcp);
	} else {
		addr.sin_port = htons(session->sendport_audio_rtcp);
	}

	// forward to the selected UDP port
	int numsent = sendto(session->sendsockfd, packet->buffer, packet->length, 0, (struct sockaddr*)&addr, addrlen);
}

void rtp2gstipc_incoming_data(janus_plugin_session *handle, janus_plugin_data *packet) {
	JANUS_LOG(LOG_INFO, "%s Got a DataChannel message (%d bytes.)\n", RTP2GSTIPC_NAME, packet->length);
}

void rtp2gstipc_slow_link(janus_plugin_session *handle, int uplink, int video) {
	JANUS_LOG(LOG_INFO, "%s Slow link detected.\n", RTP2GSTIPC_NAME);
}

void rtp2gstipc_hangup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "%s hangup media.\n", RTP2GSTIPC_NAME);
}




/* Thread to handle incoming messages */
static void *rtp2gstipc_handler_thread(void *data) {
	JANUS_LOG(LOG_VERB, "%s Starting msg handler thread\n", RTP2GSTIPC_NAME);
	rtp2gstipc_message *msg = NULL;
	int error_code = 0;
	char *error_cause = g_malloc0(512);
	json_t *body = NULL;


	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		msg = g_async_queue_pop(messages);

		if(msg == NULL)
			continue;
		if(msg == &exit_message)
			break;
		if(msg->handle == NULL) {
			rtp2gstipc_message_free(msg);
			continue;
		}

		janus_mutex_lock(&sessions_mutex);
		rtp2gstipc_session *session = (rtp2gstipc_session *)msg->handle->plugin_handle;
		if(!session) {
			janus_mutex_unlock(&sessions_mutex);
			JANUS_LOG(LOG_ERR, "%s rtp2gstipc_handler_thread: No session associated with this handle...\n", RTP2GSTIPC_NAME);
			rtp2gstipc_message_free(msg);
			continue;
		}
		if(session->destroyed) {
			janus_mutex_unlock(&sessions_mutex);
			rtp2gstipc_message_free(msg);
			continue;
		}
		janus_mutex_unlock(&sessions_mutex);

		char *jsondump;
		jsondump = json_dumps(msg->jsep, 0);
		JANUS_LOG(LOG_INFO, "%s rtp2gstipc_handler_thread JSEP %s\n", RTP2GSTIPC_NAME, jsondump);
		free(jsondump);

		jsondump = json_dumps(msg->body, 0);
		JANUS_LOG(LOG_INFO, "%s rtp2gstipc_handler_thread BODY %s\n", RTP2GSTIPC_NAME, jsondump);
		free(jsondump);

		/* Handle request */
		error_code = 0;
		body = msg->body;

		if (msg->jsep) {
			const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));

			JANUS_LOG(LOG_INFO, "%s SDP OFFER ASYNC: %s\n", RTP2GSTIPC_NAME, msg_sdp);

			char error_str[512];
			janus_sdp *offer = janus_sdp_parse(msg_sdp, error_str, sizeof(error_str));
			if(offer == NULL) {
				JANUS_LOG(LOG_ERR, "%s Error parsing offer: %s\n", RTP2GSTIPC_NAME, error_str);
				error_code = RTP2GSTIPC_ERROR_INVALID_SDP;
				g_snprintf(error_cause, 512, "Error parsing offer: %s", error_str);
				goto error;;
			}

			janus_sdp *answer = janus_sdp_generate_answer(offer,
				JANUS_SDP_OA_AUDIO, TRUE,
				JANUS_SDP_OA_AUDIO_DIRECTION, JANUS_SDP_RECVONLY,
				JANUS_SDP_OA_AUDIO_CODEC, session->negotiate_acodec,

				JANUS_SDP_OA_VIDEO, TRUE,
				JANUS_SDP_OA_VIDEO_DIRECTION, JANUS_SDP_RECVONLY,
				JANUS_SDP_OA_VIDEO_CODEC, session->negotiate_vcodec,

				JANUS_SDP_OA_DATA, FALSE,
				JANUS_SDP_OA_DONE
			);
			janus_sdp_destroy(offer);

			const char *negotiated_acodec, *negotiated_vcodec;
			negotiated_acodec = NULL;
			negotiated_vcodec = NULL;

			janus_sdp_find_first_codecs(answer, &negotiated_acodec, &negotiated_vcodec);

			if (negotiated_vcodec) {
				if (!strcmp(negotiated_vcodec, "vp8")) {
					JANUS_LOG(LOG_INFO, "%s Negotiated video codec is VP8\n", RTP2GSTIPC_NAME);
					session->vcodec = CODEC_VP8;
				} else if (!strcmp(negotiated_vcodec, "vp9")) {
					JANUS_LOG(LOG_INFO, "%s Negotiated video codec is VP9\n", RTP2GSTIPC_NAME);
					session->vcodec = CODEC_VP9;
				} else if (!strcmp(negotiated_vcodec, "h264")) {
					JANUS_LOG(LOG_INFO, "%s Negotiated video codec is H264\n", RTP2GSTIPC_NAME);
					session->vcodec = CODEC_H264;
				}
			} else {
				JANUS_LOG(LOG_INFO, "%s No video for this session\n", RTP2GSTIPC_NAME);
				session->vcodec = CODEC_NONE;
			}

			char *sdp_answer = janus_sdp_write(answer);
			janus_sdp_destroy(answer);

			const char *type = "answer";
			json_t *jsep = json_pack("{ssss}", "type", type, "sdp", sdp_answer);

			json_t *response = json_object();
			json_object_set_new(response, "rtp2gstipc", json_string("event"));
			json_object_set_new(response, "result", json_string("ok"));

			// How long will the gateway take to push the reply?
			g_atomic_int_set(&session->hangingup, 0);
			int res = gateway->push_event(msg->handle, &rtp2gstipc_plugin, msg->transaction, response, jsep);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d\n", res);
			g_free(sdp_answer);

			// The Janus core increases the references to both the message and jsep *json_t objects.
			json_decref(response);
			json_decref(jsep);

	} // if jsep in message


		rtp2gstipc_message_free(msg);

		continue;

error:
		{
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "echotest", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			int ret = gateway->push_event(msg->handle, &rtp2gstipc_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
			rtp2gstipc_message_free(msg);
			/* We don't need the event anymore */
			json_decref(event);
		}
	}
	g_free(error_cause);
	JANUS_LOG(LOG_VERB, "%s Leaving msg handler thread\n", RTP2GSTIPC_NAME);
	return NULL;
}
