/* GStreamer
 * Copyright (C) 2022 Taruntej Kanakamalla <taruntejk@live.com>
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

#include "gst/gstelement.h"
#include "gst/gstinfo.h"
#include "gst/gstpad.h"
#include "gst/gstpadtemplate.h"
#include "gst/gstparamspecs.h"
#include "gstwhipsink.h"
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
static void gst_whip_sink_state_changed (GstElement * element, GstState oldstate,
    GstState newstate, GstState pending);
// static GstStateChangeReturn gst_whip_sink_change_state (GstElement *element, 
    // GstStateChange transition);

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
_update_ice_servers (GstWhipSink * whipsink, const gchar *link)
{
  int i = 0;
  gchar **links = g_strsplit (link, ", ", -1);
  while (links[i] != NULL) {
    GST_DEBUG_OBJECT (whipsink, "%s", links[i]);
    i++;
  }
  g_clear_pointer (&links, g_strfreev);
}

static void
_configure_ice_servers_from_link_headers (GstWhipSink * whipsink)
{
  GST_DEBUG_OBJECT (whipsink, " Using link headers to get ice-servers");
  SoupMessage *msg =
      soup_message_new ("OPTIONS", (const char *) whipsink->whip_endpoint);
  guint status = soup_session_send_message (whipsink->soup_session, msg);
  if (status != 200 && status != 204) {
    GST_ERROR_OBJECT (whipsink, " [%u] %s\n\n", status,
        status ? msg->reason_phrase : "HTTP error");
    return;
  }
  GST_INFO_OBJECT (whipsink, "Updating ice servers from OPTIONS response");
  const gchar *link =
      soup_message_headers_get_list (msg->response_headers, "link");
  if (link) {
    GST_DEBUG_OBJECT (whipsink, "link headers :%s", link);
    _update_ice_servers (whipsink, link);
  }
  g_object_unref(msg);
}

static void
_send_sdp (GstWhipSink * whipsink, GstWebRTCSessionDescription * desc,
    gchar **answer)
{
  gchar *text;

  text = gst_sdp_message_as_text (desc->sdp);

  g_print ("%s : %s\n", __func__, text);
  SoupMessage *msg;
  
  msg = soup_message_new ("POST", (const char *) whipsink->whip_endpoint);
  soup_message_set_request (msg, "application/sdp", SOUP_MEMORY_COPY,
      (const char *) text, strlen (text));
  guint status = soup_session_send_message (whipsink->soup_session, msg);
  g_print ("%s soup return %d :\n%s\n", __func__, status,
      msg->response_body->data);
  if (status == 201) {
    *answer =  g_strdup(msg->response_body->data);
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
  if (whipsink->use_link_headers) {
    //update the ice-servers if they exist
    const char *link =
        soup_message_headers_get_list (msg->response_headers, "link");
    if (link == NULL) {
      GST_WARNING_OBJECT (whipsink,
          "Link headers not found in the POST response");
    } else {
      GST_INFO_OBJECT (whipsink, "Updating ice servers from POST response");
      _update_ice_servers (whipsink, link);
    }
  }
  g_object_unref(msg);
}

static void
_on_offer_created (GstPromise * promise, gpointer whipsink)
{
  GstWhipSink *ws = (GstWhipSink *) whipsink;
  gpointer webrtcbin = (gpointer) ws->webrtcbin;
  if (gst_promise_wait (promise) == GST_PROMISE_RESULT_REPLIED) {
    GstWebRTCSessionDescription *offer;
    const GstStructure *reply;

    reply = gst_promise_get_reply (promise);
    gst_structure_get (reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
        &offer, NULL);
    gst_promise_unref (promise);

    promise = gst_promise_new ();
    g_signal_emit_by_name (webrtcbin, "set-local-description", offer, promise);
    gst_promise_interrupt (promise);
    gst_promise_unref (promise);
    gchar *answer = NULL;
    _send_sdp (ws, offer, &answer);
    if (strstr (answer, "candidate") != NULL) {
      int mlines = 0, i = 0;
      gchar **lines = g_strsplit (answer, "\r\n", -1);
      gchar *line = NULL;
      while (lines[i] != NULL) {
        line = lines[i];
        if (strstr (line, "m=") == line) {
          /* New m-line */
          mlines++;
          if (mlines > 1)       /* We only need candidates from the first one */
            break;
        } else if (mlines == 1 && strstr (line, "a=candidate") != NULL) {
          /* Found a candidate, fake a trickle */
          line += 2;
          g_signal_emit_by_name (webrtcbin, "add-ice-candidate", 0, line);
        }
        i++;
      }
      g_clear_pointer (&lines, g_strfreev);
    }
    GstWebRTCSessionDescription *answer_sdp;
    GstSDPMessage *sdp_msg;
    gst_sdp_message_new_from_text (answer, &sdp_msg);
    answer_sdp =
        gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER,
        sdp_msg);
    promise = gst_promise_new ();
    g_signal_emit_by_name (webrtcbin, "set-remote-description", answer_sdp,
        promise);
    gst_promise_interrupt (promise);
    gst_promise_unref (promise);
    gst_webrtc_session_description_free (offer);
    gst_webrtc_session_description_free (answer_sdp);
    g_free(answer);
  }
}

static void
_on_negotiation_needed (GstElement * webrtcbin, gpointer user_data)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (user_data);
  GST_ERROR_OBJECT (whipsink, " whipsink: %p...webrtcbin :%p \n", whipsink,
      webrtcbin);
  GstPromise *promise =
      gst_promise_new_with_change_func (_on_offer_created, (gpointer) whipsink,
      NULL);
  g_signal_emit_by_name ((gpointer) webrtcbin, "create-offer", NULL, promise);
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
  gstelement_class->state_changed =
      GST_DEBUG_FUNCPTR (gst_whip_sink_state_changed);
  //gstelement_class->change_state =
     // GST_DEBUG_FUNCPTR(gst_whip_sink_change_state);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_whip_sink_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "WHIP Bin", "Sink/Network/WebRTC",
      "A bin for WebRTC-HTTP ingestion protocol (WHIP)",
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
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

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
  whipsink->webrtcbin = gst_element_factory_make ("webrtcbin", "whip-webrtcbin");
  gst_bin_add (GST_BIN (whipsink), whipsink->webrtcbin);
  GST_ERROR_OBJECT (whipsink, " webrtcbin :%p", whipsink->webrtcbin);
  g_signal_connect (whipsink->webrtcbin, "on-negotiation-needed",
      G_CALLBACK (_on_negotiation_needed), (gpointer) whipsink);
  whipsink->resource_url = NULL;
  whipsink->soup_session = soup_session_new ();
//   g_signal_connect (whipsink->webrtcbin, "on-ice-candidate",
//       G_CALLBACK (send_ice_candidate_message), NULL);
//   g_signal_connect (whipsink->webrtcbin, "notify::ice-gathering-state",
//       G_CALLBACK (on_ice_gathering_state_notify), NULL);

}

void
gst_whip_sink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (object);
  GST_DEBUG_OBJECT (whipsink, "...");
  switch (property_id) {
    case PROP_WHIP_ENDPOINT:
      GST_WHIP_SINK_LOCK (whipsink);
      g_free (whipsink->whip_endpoint);
      whipsink->whip_endpoint = g_value_dup_string (value);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;
    case PROP_STUN_SERVER:
      GST_WHIP_SINK_LOCK (whipsink);
      g_free (whipsink->stun_server);
      //todo: why not set the prop to webrtcbin right away?
      whipsink->stun_server = g_value_dup_string (value);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;

    case PROP_TURN_SERVER:
      GST_WHIP_SINK_LOCK (whipsink);
      g_free (whipsink->turn_server);
      whipsink->turn_server = g_value_dup_string (value);
      GST_WHIP_SINK_UNLOCK (whipsink);
      break;

    case PROP_BUNDLE_POLICY:
      GST_WHIP_SINK_LOCK (whipsink);
      whipsink->bundle_policy = g_value_get_enum (value);
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
  GST_DEBUG_OBJECT (whipsink, "...");
  switch (property_id) {
    case PROP_WHIP_ENDPOINT:
      g_value_take_string (value, whipsink->whip_endpoint);
      break;
    case PROP_STUN_SERVER:
      g_value_take_string (value, whipsink->turn_server);
      break;

    case PROP_TURN_SERVER:
      g_value_take_string (value, whipsink->stun_server);
      break;

    case PROP_BUNDLE_POLICY:
      g_value_set_enum (value, whipsink->bundle_policy);
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
    //todo sinkpad need not be a member
  sinkpad =
      gst_ghost_pad_new (gst_pad_get_name (wb_sink_pad), wb_sink_pad);
  gst_element_add_pad (GST_ELEMENT_CAST (whipsink), sinkpad);
  gst_object_unref (wb_sink_pad);
  // GstPadTemplate *wbin_pad_template = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS
  //       (whipsink->webrtcbin), "sink_%u");
  // GstPad * wbin_pad =
  //       gst_element_request_pad (whipsink->webrtcbin, wbin_pad_template, name, caps);
  //       //gst_element_request_pad_simple (whipsink->webrtcbin, "sink_%u");
  // whipsink->sinkpad = gst_ghost_pad_new_from_template (gst_pad_get_name (wbin_pad), wbin_pad, templ);
  // gst_element_add_pad (GST_ELEMENT (whipsink), whipsink->sinkpad);

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

    //todo check with change 
  GstPad *wbin_pad = gst_pad_get_peer (pad);
  gst_element_release_request_pad (whipsink->webrtcbin, wbin_pad);
  gst_object_unref (wbin_pad);
  gst_element_remove_pad (element, pad);
  GST_WHIP_SINK_UNLOCK (whipsink);
}

static void
gst_whip_sink_state_changed (GstElement * element, GstState oldstate,
    GstState newstate, GstState pending)
{
  GstWhipSink *whipsink = GST_WHIP_SINK (element);
  GST_DEBUG_OBJECT (whipsink, "oldstate %d pending state: %d newstate %d..",oldstate, pending, newstate);
  switch (newstate) {
    case GST_STATE_READY:
      if (pending == GST_STATE_VOID_PENDING &&
            oldstate == GST_STATE_NULL) {
        GST_DEBUG_OBJECT (whipsink, "current state %d", newstate);
        g_object_set(whipsink->webrtcbin, "bundle-policy", whipsink->bundle_policy, NULL);
        if (whipsink->use_link_headers) {
            //todo what if the transition is blocked longer due to network call. 
            //Think about returning ASYNC and cancel transition if there is a request to tear down the pipeline
          _configure_ice_servers_from_link_headers (whipsink);
        } else {
            g_object_set(whipsink->webrtcbin, "turn-server", whipsink->turn_server, NULL);
            g_object_set(whipsink->webrtcbin, "stun-server", whipsink->stun_server, NULL);
        }
      }
      break;
    default:
      break;
  }
}
