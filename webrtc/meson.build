plugin_c_args = ['-DHAVE_CONFIG_H']

cdata = configuration_data()
cdata.set_quoted('PACKAGE_VERSION', gst_version)
cdata.set_quoted('PACKAGE', 'gst-template-plugin')
cdata.set_quoted('GST_LICENSE', 'LGPL')
cdata.set_quoted('GST_API_VERSION', api_version)
cdata.set_quoted('GST_PACKAGE_NAME', 'GStreamer template Plug-ins')
cdata.set_quoted('GST_PACKAGE_ORIGIN', 'https://gstreamer.freedesktop.org')
configure_file(output : 'config.h', configuration : cdata)

gst_req = '>= 1.14.0'
gst_dep = dependency('gstreamer-1.0', version : gst_req,
  fallback : ['gstreamer', 'gst_dep'])
gstsdp_dep = dependency('gstreamer-sdp-1.0', version : gst_req,
    fallback : ['gst-plugins-base', 'sdp_dep'])
gstwebrtc_dep = dependency('gstreamer-webrtc-1.0', version : gst_req,
    fallback : ['gst-plugins-bad', 'gstwebrtc_dep'])

libsoup_dep = dependency('libsoup-2.4', version : '>=2.48',
    fallback : ['libsoup', 'libsoup_dep'])

webrtcext_sources = [
   'src/gst-plugin.c',
   'src/gstwhipsink.c'
]
webrtcext = library('gstwebrtcext',
    webrtcext_sources,
    c_args: plugin_c_args,
    dependencies : [gst_dep, gstsdp_dep, gstwebrtc_dep, libsoup_dep],
    install : true,
    install_dir : plugins_install_dir,
)
