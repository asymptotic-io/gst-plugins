/* GStreamer
 * Copyright (C) 2022 Taruntej Kanakamalla <taruntej@asymptotic.io>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */


/**
 * SECTION:element-gstwhipsink
 *
 * The whipsink element wraps the functionality of webrtcbin and 
 * adds the HTTP ingestion in compliance with the draft-ietf-wish-whip-01 thus 
 * supporting the WebRTC-HTTP ingestion protocol (WHIP)
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 videotestsrc is-live=true pattern=ball ! videoconvert ! queue ! vp8enc deadline=1 ! rtpvp8pay ! queue ! whipsink name=ws whip-endpoint="http://localhost:7080/whip/endpoint/abc123" use-link-headers=true bundle-policy=3 
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#include <gst/gst.h>
#include <gst/gst.h>
#include <string.h>

#include "gst/gstelement.h"
#include "gst/gstinfo.h"
#include "gst/gstpad.h"
#include "gst/gstpadtemplate.h"
#include "gst/gstparamspecs.h"
#include "gst/gstpromise.h"
#include "gstwhipsink.h"
#include "libsoup/soup-session.h"
#include "libsoup/soup-uri.h"

GST_DEBUG_CATEGORY_STATIC (gst_whip_sink_debug_category);
#define GST_CAT_DEFAULT gst_whip_sink_debug_category

#define GST_WHIP_SINK_STATE_LOCK(s) g_mutex_lock(&(s)->state_lock)
#define GST_WHIP_SINK_STATE_UNLOCK(s) g_mutex_unlock(&(s)->state_lock)

#define GST_WHIP_SINK_LOCK(s) g_mutex_lock(&(s)->lock)
#define GST_WHIP_SINK_UNLOCK(s) g_mutex_unlock(&(s)->lock)

/* prototypes */

#define gst_whip_sink_parent_class parent_class

static void gst_whip_sink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_whip_sink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_whip_sink_dispose (GObject * object);
static void gst_whip_sink_finalize (GObject * object);
static GstPad *gst_whip_sink_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_whip_sink_release_pad (GstElement * element, GstPad * pad);
static GstStateChangeReturn gst_whip_sink_change_state (GstElement * element,
    GstStateChange transition);
static void do_async_done (GstWhipSink * whipsink);
static void do_async_start (GstWhipSink * whipsink);

/* pad templates */

static GstStaticPadTemplate gst_whip_sink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS ("application/x-rtp")
    );


enum
{
  PROP_0,
  PROP_WHIP_ENDPOINT,
  PROP_STUN_SERVER,
  PROP_TURN_SERVER,
  PROP_BUNDLE_POLICY,
  PROP_USE_LINK_HEADERS,
};

static void
_update_ice_servers (GstWhipSink * whipsink, const gchar * link_header)
{
  int i = 0;
  gchar **lists = g_strsplit (link_header, ", ", -1);

  while (lists[i] != NULL) {

    GST_DEBUG_OBJECT (whipsink, "%s", lists[i]);
    gchar *ice_server = g_strstr_len (lists[i], -1, "rel=\"ice-server\"");

    if (ice_server) {
      int j = 0;
      gchar **members = g_strsplit (lists[i], "; ", -1);
      gchar *stun_svr = NULL;
      gchar *turn_svr = NULL, *turn_s_svr = NULL, *turn_user =
          NULL, *turn_pass = NULL;
      gchar *turn_cred_type = NULL;
      while (members[j] != NULL) {
        //todo can this be done using a lookup table or a hashmap

        if (0 == g_ascii_strncasecmp (members[j], "<stun:", strlen ("<stun:"))) {

          //start after leading '<stun:'
          stun_svr = g_strdup (members[j] + strlen ("<stun:"));
          //remove trailing '>'
          stun_svr[strlen (stun_svr) - 1] = '\0';

        } else if (0 == g_ascii_strncasecmp (members[j], "<turn:",
                strlen ("<turn:"))) {

          //start after leading '<turn:'
          turn_svr = g_strdup (members[j] + strlen ("<turn:"));
          //remove trailing '>'
          turn_svr[strlen (turn_svr) - 1] = '\0';

        } else if (0 == g_ascii_strncasecmp (members[j], "<turns:",
                strlen ("<turns:"))) {

          //start after leading '<turn:'
          turn_s_svr = g_strdup (members[j] + strlen ("<turns:"));
          //remove trailing '>'
          turn_s_svr[strlen (turn_s_svr) - 1] = '\0';

        } else if (0 == g_ascii_strncasecmp (members[j], "username=\"",
                strlen ("username=\""))) {

          //start after leading '"'
          turn_user = g_strdup (members[j] + strlen ("username=\""));
          //remove trailing '"'
          turn_user[strlen (turn_user) - 1] = '\0';

        } else if (0 == g_ascii_strncasecmp (members[j], "credential=\"",
                strlen ("credential=\""))) {

          //start after leading '"'
          turn_pass = g_strdup (members[j] + strlen ("credential=\""));
          //remove trailing '"'
          turn_pass[strlen (turn_pass) - 1] = '\0';

        } else if (0 == g_ascii_strncasecmp (members[j], "credential-type: \"",
                strlen ("credential-type: \""))) {

          //start after leading '"'
          turn_cred_type =
              g_strdup (members[j] + strlen ("credential-type: \""));
          //remove trailing '"'
          turn_cred_type[strlen (turn_cred_type) - 1] = '\0';

        }
        j++;
      }

      if (stun_svr) {

        gchar *stun_url = g_strdup_printf ("stun://%s", stun_svr);
        GST_DEBUG_OBJECT (whipsink, "stun url %s", stun_url);

        //this overwrites the stun-server value set by set_property
        g_object_set (whipsink->webrtcbin, "stun-server", stun_url, NULL);
        g_free (stun_url);

      } else if (turn_svr) {

        if (!g_ascii_strncasecmp (turn_cred_type, "password",
                strlen ("password")) && turn_user && turn_pass) {
          gchar *turn_url =
              g_strdup_printf ("turn://%s:%s@%s", turn_user, turn_pass,
              turn_svr);

          gboolean retval;
          GST_DEBUG_OBJECT (whipsink, "turn url %s", turn_url);
          g_signal_emit_by_name (whipsink->webrtcbin, "add-turn-server",
              turn_url, &retval);
          if (!retval)
            GST_ERROR_OBJECT (whipsink, "failed to add-turn-server %s",
                turn_url);
          g_free (turn_url);

        }
      } else if (turn_s_svr) {

        if (!g_ascii_strncasecmp (turn_cred_type, "password",
                strlen ("password")) && turn_user && turn_pass) {
          gchar *turn_s_url =
              g_strdup_printf ("turns://%s:%s@%s", turn_user, turn_pass,
              turn_s_svr);
          gboolean retval;
          GST_DEBUG_OBJECT (whipsink, "turns url %s", turn_s_url);
          g_signal_emit_by_name (whipsink->webrtcbin, "add-turn-server",
              turn_s_url, &retval);
          if (!retval)
            GST_ERROR_OBJECT (whipsink, "failed to add-turn-server %s",
                turn_s_url);
          g_free (turn_s_url);

        }
      }
      g_strfreev (members);
      g_free (stun_svr);
      g_free (turn_svr);
      g_free (turn_s_svr);
      g_free (turn_user);
      g_free (turn_pass);
      g_free (turn_cred_type);
    }
    i++;
  }
  g_strfreev (lists);
}

static void
_send_sdp (GstWhipSink * whipsink, GstWebRTCSessionDescription * desc,
    gchar ** answer)
{
  gchar *text;

  text = gst_sdp_message_as_text (desc->sdp);

  GST_DEBUG_OBJECT (whipsink, "...\n%s", text);
  SoupMessage *msg;

  msg = soup_message_new ("POST", (const char *) whipsink->whip_endpoint);
  soup_message_set_request (msg, "application/sdp", SOUP_MEMORY_COPY,
      (const char *) text, strlen (text));
  guint status = soup_session_send_message (whipsink->soup_session, msg);
  GST_DEBUG_OBJECT (whipsink, "msg status %u \n%s", status,
      msg->response_body->data);
  if (status == 201) {
    *answer = g_strdup (msg->response_body->data);
  } else {
    //todo handle else case
    return;
  }
  const char *location =
      soup_message_headers_get_one (msg->response_headers, "location");
  if (location != NULL && location[0] == '/') {
    SoupURI *uri = soup_uri_new (whipsink->whip_endpoint);
    soup_uri_set_path (uri, location);
    whipsink->resource_url = soup_uri_to_string (uri, FALSE);
    GST_DEBUG_OBJECT (whipsink, "resource url is %s", whipsink->resource_url);
    soup_uri_free (uri);
  }
  // if (whipsink->use_link_headers) {
  //   //update the ice-servers if they exist
  //   const char *link_header =
  //       soup_message_headers_get_list (msg->response_headers, "link");
  //   if (link_header == NULL) {
  //     GST_WARNING_OBJECT (whipsink,
  //         "Link headers not found in the POST response");
  //   } else {
  //     GST_INFO_OBJECT (whipsink, "Updating ice servers from POST response - %s",
  //         link_header);
  //     _update_ice_servers (whipsink, link_header);
  //   }
  // }
  g_object_unref (msg);
}


static void
_on_offer_created (GstPromise * promise, gpointer userdata)
{
  GstWhipSink *ws = GST_WHIP_SINK (userdata);
  gpointer webrtcbin = ws->webrtcbin;
  if (gst_promise_wait (promise) == GST_PROMISE_RESULT_REPLIED) {
    GstWebRTCSessionDescription *offer;
    const GstStructure *reply;

    reply = gst_promise_get_reply (promise);
    gst_structure_get (reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
        &offer, NULL);
    gst_promise_unref (promise);
    //todo add ice candidates from the ice-server
    g_signal_emit_by_name (webrtcbin, "set-local-description", offer, NULL);
    gchar *answer = NULL;
    _send_sdp (ws, offer, &answer);
    if (answer == NULL)
      return;
    GstWebRTCSessionDescription *answer_sdp;
    GstSDPMessage *sdp_msg;
    gst_sdp_message_new_from_text (answer, &sdp_msg);
    answer_sdp =
        gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER,
        sdp_msg);
    g_signal_emit_by_name (webrtcbin, "set-remote-description", answer_sdp,
        NULL);
    gst_webrtc_session_description_free (offer);
    gst_webrtc_session_description_free (answer_sdp);
    g_free (answer);
  }
}

static void
_http_options_response_callback (SoupSession * session, SoupMessage * msg,
    gpointer userdata)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (userdata);
  if (msg->status_code != 200 && msg->status_code != 204) {
    GST_DEBUG_OBJECT (whipsink, " [%u] %s\n\n", msg->status_code,
        msg->status_code ? msg->reason_phrase : "HTTP error");
  } else {
    GST_INFO_OBJECT (whipsink, "Updating ice servers from OPTIONS response");
    const gchar *link_header =
        soup_message_headers_get_list (msg->response_headers, "link");
    if (link_header) {
      GST_DEBUG_OBJECT (whipsink, "link headers :%s", link_header);
      _update_ice_servers (whipsink, link_header);
    }

    GstPromise *promise = gst_promise_new_with_change_func (_on_offer_created,
        (gpointer) whipsink,
        NULL);
    g_signal_emit_by_name ((gpointer) whipsink->webrtcbin, "create-offer", NULL,
        promise);
  }
}

static void
_configure_ice_servers_from_link_headers (GstWhipSink * whipsink,
    gboolean async)
{
  GST_DEBUG_OBJECT (whipsink, " Using link headers to get ice-servers");
  SoupMessage *msg =
      soup_message_new ("OPTIONS", (const char *) whipsink->whip_endpoint);
  if (async) {
    soup_session_queue_message (whipsink->soup_session, msg,
        _http_options_response_callback, whipsink);
  } else {
    guint status = soup_session_send_message (whipsink->soup_session, msg);
    if (status != 200 && status != 204) {
      GST_DEBUG_OBJECT (whipsink, " [%u] %s\n\n", status,
          status ? msg->reason_phrase : "HTTP error");
    } else {
      GST_INFO_OBJECT (whipsink, "Updating ice servers from OPTIONS response");
      const gchar *link_header =
          soup_message_headers_get_list (msg->response_headers, "link");
      if (link_header) {
        GST_DEBUG_OBJECT (whipsink, "link headers :%s", link_header);
        _update_ice_servers (whipsink, link_header);
      }

      GstPromise *promise = gst_promise_new_with_change_func (_on_offer_created,
          (gpointer) whipsink,
          NULL);
      g_signal_emit_by_name ((gpointer) whipsink->webrtcbin, "create-offer",
          NULL, promise);
    }
    g_object_unref (msg);
  }
}

static void
_on_negotiation_needed (GstElement * webrtcbin, gpointer user_data)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (user_data);
  GST_DEBUG_OBJECT (whipsink, " whipsink: %p...webrtcbin :%p \n", whipsink,
      webrtcbin);
  if (whipsink->use_link_headers)
    _configure_ice_servers_from_link_headers (whipsink, TRUE);
  else {
    GstPromise *promise = gst_promise_new_with_change_func (_on_offer_created,
        (gpointer) whipsink,
        NULL);
    g_signal_emit_by_name ((gpointer) webrtcbin, "create-offer", NULL, promise);
  }
}

static void
_gather_ice_candidate (GstElement * webrtc G_GNUC_UNUSED, guint mlineindex,
    gchar * candidate, gpointer user_data G_GNUC_UNUSED)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (user_data);
  GST_DEBUG_OBJECT (whipsink, "%u : %s", mlineindex, candidate);
  //todo add ice candidate to the queue
}

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstWhipSink, gst_whip_sink, GST_TYPE_BIN,
    GST_DEBUG_CATEGORY_INIT (gst_whip_sink_debug_category, "whipsink", 0,
        "debug category for whipsink element"));


static void
gst_whip_sink_class_init (GstWhipSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  //GstBinClass *gstbin_class = GST_BIN_CLASS (klass);

  gobject_class->set_property = gst_whip_sink_set_property;
  gobject_class->get_property = gst_whip_sink_get_property;
  gobject_class->dispose = gst_whip_sink_dispose;
  gobject_class->finalize = gst_whip_sink_finalize;
  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_whip_sink_request_new_pad);
  gstelement_class->release_pad = GST_DEBUG_FUNCPTR (gst_whip_sink_release_pad);
  // gstelement_class->change_state =
  //     GST_DEBUG_FUNCPTR(gst_whip_sink_change_state);

  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_whip_sink_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "WHIP Bin", "Sink/Network/WebRTC",
      "A bin for WebRTC HTTP ingestion protocol (WHIP)",
      "Taruntej Kanakamalla <taruntej@asymptotic.io>");

  g_object_class_install_property (gobject_class,
      PROP_WHIP_ENDPOINT,
      g_param_spec_string ("whip-endpoint", "WHIP Endpoint",
          "The WHIP server endpoint to POST SDP offer. "
          "e.g.: https://example.com/whip/endpoint/room1234",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
          | GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class,
      PROP_STUN_SERVER,
      g_param_spec_string ("stun-server", "STUN Server",
          "The STUN server of the form stun://hostname:port",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
          | GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class,
      PROP_TURN_SERVER,
      g_param_spec_string ("turn-server", "TURN Server",
          "The TURN server of the form turn(s)://username:password@host:port",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
          | GST_PARAM_MUTABLE_READY));

  //todo: check bundle-policy is impacted by janus as well
  g_object_class_install_property (gobject_class,
      PROP_BUNDLE_POLICY,
      g_param_spec_enum ("bundle-policy", "Bundle Policy",
          "The policy to apply for bundling",
          GST_TYPE_WEBRTC_BUNDLE_POLICY,
          GST_WEBRTC_BUNDLE_POLICY_NONE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class,
      PROP_USE_LINK_HEADERS,
      g_param_spec_boolean ("use-link-headers", "Use Link Headers",
          "Use Link Headers to cofigure ice-servers in the response from WHIP server. "
          "If set to TRUE and the WHIP server returns valid ice-servers, "
          "this property overrides the ice-servers values set using the stun-server and turn-server properties.",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
          | GST_PARAM_MUTABLE_READY));

}

static void
gst_whip_sink_init (GstWhipSink * whipsink)
{
  whipsink->webrtcbin =
      gst_element_factory_make ("webrtcbin", "whip-webrtcbin");
  gst_bin_add (GST_BIN (whipsink), whipsink->webrtcbin);
  g_signal_connect (whipsink->webrtcbin, "on-negotiation-needed",
      G_CALLBACK (_on_negotiation_needed), (gpointer) whipsink);

  // GstWebRTCRTPTransceiverDirection direction, trans_direction;
  // GstWebRTCRTPTransceiver *trans;

  // direction = GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY;
  // g_signal_emit_by_name (whipsink->webrtcbin, "add-transceiver", direction, NULL,
  //     &trans);
  // g_object_get (trans, "direction", &trans_direction, NULL);
  // GST_DEBUG_OBJECT(whipsink, "new trans direction %d", trans_direction);
  whipsink->resource_url = NULL;
  whipsink->soup_session = soup_session_new_with_options ("timeout", 30, NULL);
  // g_signal_connect (whipsink->webrtcbin, "on-ice-candidate",
  //     G_CALLBACK (_gather_ice_candidate);
  // g_signal_connect (whipsink->webrtcbin, "notify::ice-gathering-state",
  //     G_CALLBACK (_on_ice_gathering_state_change), whipsink);

}

void
gst_whip_sink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (object);
  GST_DEBUG_OBJECT (whipsink, "property_id %d", property_id);
  switch (property_id) {
    case PROP_WHIP_ENDPOINT:
      GST_WHIP_SINK_LOCK (whipsink);
      g_free (whipsink->whip_endpoint);
      whipsink->whip_endpoint = g_value_dup_string (value);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;
    case PROP_STUN_SERVER:
      GST_WHIP_SINK_LOCK (whipsink);
      g_object_set (whipsink->webrtcbin, "stun-server",
          g_value_dup_string (value), NULL);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;

    case PROP_TURN_SERVER:
      GST_WHIP_SINK_LOCK (whipsink);
      g_object_set (whipsink->webrtcbin, "turn-server",
          g_value_dup_string (value), NULL);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;

    case PROP_BUNDLE_POLICY:
      GST_WHIP_SINK_LOCK (whipsink);
      g_object_set (whipsink->webrtcbin, "bundle-policy",
          g_value_get_enum (value), NULL);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;

    case PROP_USE_LINK_HEADERS:
      GST_WHIP_SINK_LOCK (whipsink);
      whipsink->use_link_headers = g_value_get_boolean (value);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_whip_sink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (object);

  GST_DEBUG_OBJECT (whipsink, "prop id %d", property_id);

  switch (property_id) {
    case PROP_WHIP_ENDPOINT:
      g_value_take_string (value, g_strdup (whipsink->whip_endpoint));
      break;
    case PROP_STUN_SERVER:
    {
      gchar *stun_svr = NULL;
      g_object_get (whipsink->webrtcbin, "stun-server", &stun_svr, NULL);
      g_value_take_string (value, stun_svr);
    }

      break;
    case PROP_TURN_SERVER:
    {
      gchar *turn_svr = NULL;
      g_object_get (whipsink->webrtcbin, "turn-server", &turn_svr, NULL);
      g_value_take_string (value, turn_svr);
    }
      break;

    case PROP_BUNDLE_POLICY:
    {
      GstWebRTCBundlePolicy bundle_policy;
      g_object_get (whipsink->webrtcbin, "bundle-policy", &bundle_policy, NULL);
      g_value_set_enum (value, bundle_policy);
    }
      break;

    case PROP_USE_LINK_HEADERS:
      g_value_set_boolean (value, whipsink->use_link_headers);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }

}

void
gst_whip_sink_dispose (GObject * object)
{

  GstWhipSink *whipsink = GST_WHIP_SINK (object);
  SoupMessage *msg;

  if (whipsink->resource_url && whipsink->soup_session) {
    msg = soup_message_new ("DELETE", whipsink->resource_url);
    guint status = soup_session_send_message (whipsink->soup_session, msg);
    g_print ("%s delete return %d : %s\n", __func__, status,
        msg->response_body->data);
    g_object_unref (msg);
  }

  g_object_unref ((gpointer) whipsink->soup_session);
  g_free (whipsink->resource_url);
  G_OBJECT_CLASS (parent_class)->dispose (object);

}

void
gst_whip_sink_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstPad *
gst_whip_sink_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (element);
  GstPad *sinkpad;
  GST_DEBUG_OBJECT (whipsink, "templ:%s, name:%s", templ->name_template, name);

  GST_WHIP_SINK_LOCK (whipsink);
  GstPad *wb_sink_pad =
      gst_element_request_pad_simple (whipsink->webrtcbin, "sink_%u");
  sinkpad = gst_ghost_pad_new (gst_pad_get_name (wb_sink_pad), wb_sink_pad);
  gst_element_add_pad (GST_ELEMENT_CAST (whipsink), sinkpad);
  gst_object_unref (wb_sink_pad);
  GST_WHIP_SINK_UNLOCK (whipsink);
  return sinkpad;
}

static void
gst_whip_sink_release_pad (GstElement * element, GstPad * pad)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (element);
  GST_DEBUG_OBJECT (whipsink, "releasing request pad");
  GST_INFO_OBJECT (pad, "releasing request pad");
  GST_WHIP_SINK_LOCK (whipsink);

  GstPad *wbin_pad = gst_pad_get_peer (pad);
  gst_element_release_request_pad (whipsink->webrtcbin, wbin_pad);
  gst_object_unref (wbin_pad);
  gst_element_remove_pad (element, pad);
  GST_WHIP_SINK_UNLOCK (whipsink);
}

static void
do_async_start (GstWhipSink * whipsink)
{
  if (!whipsink->do_async) {
    GstMessage *msg = gst_message_new_async_start (GST_OBJECT_CAST (whipsink));

    GST_DEBUG_OBJECT (whipsink, "Posting async-start");
    GST_BIN_CLASS (parent_class)->handle_message (GST_BIN_CAST (whipsink), msg);
    whipsink->do_async = TRUE;
  }
}

static void
do_async_done (GstWhipSink * whipsink)
{
  if (whipsink->do_async) {
    GstMessage *msg = gst_message_new_async_done (GST_OBJECT_CAST (whipsink),
        GST_CLOCK_TIME_NONE);

    GST_DEBUG_OBJECT (whipsink, "Posting async-done");
    GST_BIN_CLASS (parent_class)->handle_message (GST_BIN_CAST (whipsink), msg);
    whipsink->do_async = FALSE;
  }
}

static GstStateChangeReturn
gst_whip_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret_state = GST_STATE_CHANGE_SUCCESS;
  GstWhipSink *whipsink = GST_WHIP_SINK (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      // GST_WHIP_SINK_LOCK (whipsink);
      // _configure_ice_servers_from_link_headers(whipsink, TRUE);
      // do_async_start (whipsink);
      // GST_WHIP_SINK_UNLOCK (whipsink);
      // ret_state = GST_STATE_CHANGE_ASYNC;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      // GST_WHIP_SINK_LOCK (whipsink);
      // _configure_ice_servers_from_link_headers(whipsink, TRUE);
      // do_async_start (whipsink);
      // GST_WHIP_SINK_UNLOCK (whipsink);
      // ret_state = GST_STATE_CHANGE_ASYNC;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    case GST_STATE_CHANGE_NULL_TO_NULL:
    case GST_STATE_CHANGE_READY_TO_READY:
    case GST_STATE_CHANGE_PAUSED_TO_PAUSED:
    case GST_STATE_CHANGE_PLAYING_TO_PLAYING:
    default:
      break;
  }

  GstStateChangeReturn ret =
      GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    GST_WHIP_SINK_LOCK (whipsink);
    // do_async_done (whipsink);
    GST_WHIP_SINK_UNLOCK (whipsink);
    return ret;
  }

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_WHIP_SINK_LOCK (whipsink);
      // do_async_done (whipsink);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      //todo cancel/finish soup call if pending
    default:
      break;
  }

  return ret_state;
}
