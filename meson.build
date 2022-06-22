project('gst-template', 'c', version : '1.20.0.1', license : 'LGPL')

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

cc = meson.get_compiler('c')

gst_version = meson.project_version()

api_version = '1.0'

gst_dep = dependency('gstreamer-1.0', version : '>=1.20',
    required : true, fallback : ['gstreamer', 'gst_dep'])
gstbase_dep = dependency('gstreamer-base-1.0', version : '>=1.20',
  fallback : ['gstreamer', 'gst_base_dep'])
gstbad_dep = dependency('gstreamer-plugins-bad-1.0', version : '>=1.20',
  fallback : ['gstreamer', 'gst_bad_dep'])

#FIXME: unable to gstreamer-good-1.0.pc. Need to find out
#gstgood_dep = dependency('gstreamer-plugins-good-1.0', version : '>=1.20',
#  fallback : ['gstreamer', 'gst_good_dep'])

gstaudio_dep = dependency('gstreamer-audio-1.0',
  fallback : ['gst-plugins-base', 'audio_dep'])

gstvideo_dep = dependency('gstreamer-video-1.0',
    method : 'pkg-config')

subdir('webrtc')
