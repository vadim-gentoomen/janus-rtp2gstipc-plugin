//
// Created by vadim on 23.12.2020.
//

#ifndef JANUS_RTP2GSTIPC_PLUGIN_JANUS_RTP2GSTIPC_H
#define JANUS_RTP2GSTIPC_PLUGIN_JANUS_RTP2GSTIPC_H

#include <jansson.h>
#include <plugins/plugin.h>
#include <debug.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include <poll.h>

#include "debug.h"
#include "apierror.h"
#include "config.h"
#include "mutex.h"

#include "rtp.h"
#include "rtcp.h"
#include "sdp-utils.h"
#include "utils.h"

#include "rtp2gst.h"

#include <gst/gst.h>
#include <gst/rtp/rtp.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#define RTP2GSTIPC_VERSION 1
#define RTP2GSTIPC_VERSION_STRING	"0.0.1"
#define RTP2GSTIPC_DESCRIPTION "Forwards RTP and RTCP to gstreamer appsrc"
#define RTP2GSTIPC_NAME "rtp2gstipc"
#define RTP2GSTIPC_AUTHOR	"Vadim Dynnik"
#define RTP2GSTIPC_PACKAGE "janus.plugin.rtp2gstipc"

janus_plugin *create(void);
int rtp2gstipc_init(janus_callbacks *callback, const char *config_path);
void rtp2gstipc_destroy(void);
int rtp2gstipc_get_api_compatibility(void);
int rtp2gstipc_get_version(void);
const char *rtp2gstipc_get_version_string(void);
const char *rtp2gstipc_get_description(void);
const char *rtp2gstipc_get_name(void);
const char *rtp2gstipc_get_author(void);
const char *rtp2gstipc_get_package(void);
void rtp2gstipc_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *rtp2gstipc_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
void rtp2gstipc_setup_media(janus_plugin_session *handle);
void rtp2gstipc_incoming_rtp(janus_plugin_session *handle, janus_plugin_rtp *packet);
void rtp2gstipc_incoming_rtcp(janus_plugin_session *handle, janus_plugin_rtcp *packet);
void rtp2gstipc_incoming_data(janus_plugin_session *handle, janus_plugin_data *packet);
void rtp2gstipc_slow_link(janus_plugin_session *handle, int uplink, int video);
void rtp2gstipc_hangup_media(janus_plugin_session *handle);
void rtp2gstipc_destroy_session(janus_plugin_session *handle, int *error);
json_t *rtp2gstipc_query_session(janus_plugin_session *handle);



typedef struct rtp2gstipc_message {
    janus_plugin_session *handle;
    char *transaction;
    json_t *body;
    json_t *jsep;
} rtp2gstipc_message;

static GAsyncQueue *messages = NULL;
static rtp2gstipc_message exit_message;

#define RTP2GSTIPC_CODEC_STR_LEN 10

typedef enum rtp2gstipc_video_codec {
    CODEC_NONE,
    CODEC_VP8,
    CODEC_VP9,
    CODEC_H264
} rtp2gstipc_video_codec;

typedef struct rtp2gstipc_session {
    janus_plugin_session *handle;

    GThread *relay_thread;

    guint16 sendport_video_rtp;
    guint16 sendport_video_rtcp;
    guint16 sendport_audio_rtp;
    guint16 sendport_audio_rtcp;
    guint16 seqnr_video_last; // to keep track of lost packets
    guint16 seqnr_audio_last; // to keep track of lost packets
    guint16 drop_permille;
    guint16 drop_video_packets;
    guint16 drop_audio_packets;

    int fir_seqnr;
    int sendsockfd; // one socket for sento() several ports is enough
    struct sockaddr_in sendsockaddr;

    rtp2gstipc_video_codec vcodec;

    char negotiate_acodec[RTP2GSTIPC_CODEC_STR_LEN];
    char negotiate_vcodec[RTP2GSTIPC_CODEC_STR_LEN];

    gboolean video_enabled;
    gboolean audio_enabled;
    gboolean enable_video_on_keyframe;
    gboolean disable_video_on_packetloss;

    janus_rtp_switching_context context;
    volatile gint hangingup;
    volatile gint destroyed;
    janus_refcount ref;

    GstPipeline *pipeline;
    GstAppSrc *a_appsrc;
    GstAppSrc *v_appsrc;
    GstAppSrc *a_rtcp_appsrc;
    GstAppSrc *v_rtcp_appsrc;

} rtp2gstipc_session;

//void rtp2gst(rtp2gstipc_session *session);
//void janus_init_gstreamer(rtp2gstipc_session *session);

#endif //JANUS_RTP2GSTIPC_PLUGIN_JANUS_RTP2GSTIPC_H
