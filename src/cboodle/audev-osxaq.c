/* Boodler: a programmable soundscape tool
   Copyright 2001-2011 by Andrew Plotkin <erkyrath@eblong.com>
   Boodler web site: <http://boodler.org/>
   The cboodle_osxaq extension is distributed under the LGPL.
   See the LGPL document, or the above URL, for details.

   AudioQueue driver based on code contributed by Aaron Griffith.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include <AudioToolbox/AudioQueue.h>
/* "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/Headers/AudioQueue.h" */
#include <CoreAudio/CoreAudio.h>
/* "/System/Library/Frameworks/CoreAudio.framework/Versions/A/Headers/CoreAudio.h" */

#include "common.h"
#include "audev.h"

#define DEFAULT_SOUNDRATE (44100)
#define NUM_BUFFERS (3)
#define SIZE_BUFFERS (32768)

#define LEN_DEVICE_NAME (128)
#define LEN_DEVICE_LIST (16)

static int sound_big_endian = 0;
static long sound_rate = 0; /* frames per second */
static int sound_channels = 0;
static int sound_format = 0; /* TRUE for big-endian, FALSE for little */
static long sound_buffersize = 0; /* bytes */

static long samplesperbuf = 0;
static long framesperbuf = 0;

static long *valbuffer = NULL;

static int bufcount = NUM_BUFFERS;

typedef struct buffer_struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  AudioQueueBuffer *buffer;
  int full;
} buffer_t;

typedef struct airplay_source_struct {
    char name[LEN_DEVICE_NAME];
    UInt32 id;
} airplay_source;

static AudioQueueRef aqueue = NULL;
static buffer_t *buffers = NULL; /* array, len bufcount */

static int running = FALSE;
static int bailing = FALSE;
static int filling = 0;
static int started = FALSE;

static void playCallback(void *user, AudioQueueRef queue, 
  AudioQueueBuffer *qbuf);

int audev_init_device(char *wantdevname, long ratewanted, int verbose, extraopt_t *extra)
{
  int channels, format, rate, ix, res;
  int fragsize;
  int listdevices = FALSE;
  AudioDeviceID wantdevid;
  AudioDeviceID wantedaudev;
  AudioDeviceID wantedsourceid;
  AudioDeviceID foundsourceid = NULL;

  char devicename[LEN_DEVICE_NAME];
  char sourcename[LEN_DEVICE_NAME] = "";
  CFStringRef deviceuid = NULL;
  extraopt_t *opt;
  char endtest[sizeof(unsigned int)];
  AudioStreamBasicDescription formataq;
  OSStatus status;

  if (verbose) {
    printf("Boodler: OSX AudioQueue driver.\n");
  }

  if (running) {
    fprintf(stderr, "Sound device is already open.\n");
    return FALSE;
  }

  *((unsigned int *)endtest) = ( (unsigned int) ( ('E' << 24) + ('N' << 16) + ('D' << 8) + ('I') ) );
  if (endtest[0] == 'I' && endtest[1] == 'D' && endtest[2] == 'N' && endtest[3] == 'E') {
    sound_big_endian = FALSE;
  }
  else if (endtest[sizeof(unsigned int)-1] == 'I' && endtest[sizeof(unsigned int)-2] == 'D' && endtest[sizeof(unsigned int)-3] == 'N' && endtest[sizeof(unsigned int)-4] == 'E') {
    sound_big_endian = TRUE;
  }
  else {
    fprintf(stderr, "Cannot determine native endianness.\n");
    return FALSE;
  }

  format = -1;
  fragsize = SIZE_BUFFERS;

  for (opt=extra; opt->key; opt++) {
    if (!strcmp(opt->key, "end") && opt->val) {
      if (!strcmp(opt->val, "big"))
        format = TRUE;
      else if (!strcmp(opt->val, "little"))
        format = FALSE;
    }
    else if (!strcmp(opt->key, "buffersize") && opt->val) {
      fragsize = atol(opt->val);
    }
    else if (!strcmp(opt->key, "buffercount") && opt->val) {
      bufcount = atoi(opt->val);
    }
    else if (!strcmp(opt->key, "listdevices")) {
      listdevices = TRUE;
    }
    else if (!strcmp(opt->key, "source") && opt->val) {
	strncpy(sourcename, opt->val, LEN_DEVICE_NAME);
    }
  }

  if (bufcount < 2)
    bufcount = 2;

  wantedaudev = kAudioDeviceUnknown;

  /* If the given device name is a string representation of an
     integer, work out the integer. */
  wantdevid = kAudioDeviceUnknown;
  if (wantdevname) {
    char *endptr = NULL;
    wantdevid = strtol(wantdevname, &endptr, 10);
    if (!endptr || endptr == wantdevname || (*endptr != '\0'))
      wantdevid = kAudioDeviceUnknown;

  }

  if (sourcename) {
    char *endptr = NULL;
    wantedsourceid = strtol(sourcename, &endptr, 10);
    if (!endptr || endptr == sourcename || (*endptr != '\0'))
	wantedsourceid = NULL;
  }

  if (listdevices || wantdevname) {
    int ix, jx;
    int device_count;
    UInt32 propsize;
    airplay_source *airplay_sources = NULL;
    AudioDeviceID devicelist[LEN_DEVICE_LIST];  

    int source_count;
    unsigned int transportType = 0;
    UInt32 fetchpropsize = sizeof(transportType);
    AudioObjectPropertyAddress addr;

    propsize = LEN_DEVICE_LIST * sizeof(AudioDeviceID);
    status = AudioHardwareGetProperty(kAudioHardwarePropertyDevices,
      &propsize, devicelist);
    if (status) {
      fprintf(stderr, "Could not get list of audio devices.\n");
      return FALSE;
    }
    device_count = propsize / sizeof(AudioDeviceID);

    for (ix=0; ix<device_count; ix++) {
      
      AudioDeviceID tmpaudev = devicelist[ix];

      /* Determine if this is an output device. */
      status = AudioDeviceGetPropertyInfo(tmpaudev, 0, 0, 
	kAudioDevicePropertyStreamConfiguration, &propsize, NULL);
      if (status) {
	fprintf(stderr, "Could not get audio property info.\n");
	return FALSE;
      }

      AudioBufferList *buflist = (AudioBufferList *)malloc(propsize);
      status = AudioDeviceGetProperty(tmpaudev, 0, 0, 
	kAudioDevicePropertyStreamConfiguration, &propsize, buflist);
      if (status) {
	fprintf(stderr, "Could not get audio property info.\n");
	return FALSE;
      }

      int hasoutput = FALSE;

      for (jx=0; jx<buflist->mNumberBuffers; jx++) {
	if (buflist->mBuffers[jx].mNumberChannels > 0) {
	  hasoutput = TRUE;
	}
      }

      free(buflist);
      buflist = NULL;

      if (!hasoutput) {
	/* skip this device. */
	continue;
      }

      /* Determine the device name. */

      propsize = LEN_DEVICE_NAME * sizeof(char);
      status = AudioDeviceGetProperty(tmpaudev, 1, 0,
	kAudioDevicePropertyDeviceName, &propsize, devicename);
      if (status) {
	fprintf(stderr, "Could not get audio device name.\n");
	return FALSE;
      }

      addr.mSelector = kAudioDevicePropertyTransportType;
      addr.mScope = kAudioObjectPropertyScopeGlobal;
      addr.mElement = kAudioObjectPropertyElementMaster;
      AudioObjectGetPropertyData(tmpaudev, &addr, 0, NULL, &fetchpropsize, &transportType);

      if (kAudioDeviceTransportTypeAirPlay == transportType) {
	  UInt32 *airplay_source_ids;
	  addr.mSelector = kAudioDevicePropertyDataSources;
	  addr.mScope = kAudioDevicePropertyScopeOutput;
	  addr.mElement = kAudioObjectPropertyElementMaster;
	  AudioObjectGetPropertyDataSize(tmpaudev, &addr, 0, NULL, &fetchpropsize);
	  airplay_source_ids = malloc(fetchpropsize);
	  source_count = fetchpropsize / sizeof(UInt32);
	  airplay_sources = malloc(source_count * sizeof(airplay_source));

	  AudioObjectGetPropertyData(tmpaudev, &addr, 0, NULL, &fetchpropsize, airplay_source_ids);
	  for (int sid=0; sid < source_count; sid++) {
	      airplay_sources[sid].id = NULL;
	      int _sourceID = airplay_source_ids[sid];
	      addr.mSelector = kAudioDevicePropertyDataSourceNameForIDCFString;
	      addr.mScope = kAudioObjectPropertyScopeOutput;
	      addr.mElement = kAudioObjectPropertyElementMaster;
		
	      CFStringRef value = NULL;

	      AudioValueTranslation audioValueTranslation;
	      audioValueTranslation.mInputDataSize = sizeof(UInt32);
	      audioValueTranslation.mOutputData = (void *) &value;
	      audioValueTranslation.mOutputDataSize = sizeof(CFStringRef);
	      audioValueTranslation.mInputData = (void *) &_sourceID;

	      fetchpropsize = sizeof(AudioValueTranslation);
	      AudioObjectGetPropertyData(tmpaudev, &addr, 0, NULL, &fetchpropsize, &audioValueTranslation);
	      if (value) {
		  CFDataRef data;
		  data = CFStringCreateExternalRepresentation(NULL, value, kCFStringEncodingMacRoman, '?');
		  if (data) {
		      airplay_sources[sid].id = _sourceID;
		      strncpy(airplay_sources[sid].name, CFDataGetBytePtr(data), LEN_DEVICE_NAME);
		      CFRelease(data);
		  }
		  CFRelease(value);
	      }
	  }
	  free(airplay_source_ids);
      }

      if (listdevices) {
	printf("Found device ID %d: \"%s\".\n", (int)tmpaudev, devicename);
	if (kAudioDeviceTransportTypeAirPlay == transportType) {
	    //printf("Hey, an airplay.  Looks like %d sources...\n", source_count);
	  for (int sid=0; sid < source_count; sid++) {
	      airplay_source source = airplay_sources[sid];
	      if (source.id) {
		  printf("  - airplay source ID %d: -D source=\"%s\"\n", source.id, source.name);
	      }
	  }
	}
      }

      /* Check if the desired name matches (a prefix of) the device name. */
      if (wantdevname && !strncmp(wantdevname, devicename, 
	    strlen(wantdevname))) {
	wantedaudev = tmpaudev;
      }

      /* Check if the int version of the desired name matches the device ID. */
      if (wantdevid != kAudioDeviceUnknown && wantdevid == tmpaudev) {
	wantedaudev = tmpaudev;
      }
      if (wantedaudev == tmpaudev && kAudioDeviceTransportTypeAirPlay == transportType) {
	  if (sourcename && strnlen(sourcename, LEN_DEVICE_NAME) > 0) {
	      for (int sid=0; sid < source_count; sid++) {
		  if (airplay_sources[sid].id == NULL)
		      continue;
		  if (wantedsourceid == airplay_sources[sid].id) {
		      foundsourceid = airplay_sources[sid].id;
		      strncpy(sourcename, airplay_sources[sid].name, LEN_DEVICE_NAME);
		      break;
		  }
		  if (!strncasecmp(sourcename, airplay_sources[sid].name, strlen(sourcename))) {
		      foundsourceid = airplay_sources[sid].id;
		      strncpy(sourcename, airplay_sources[sid].name, LEN_DEVICE_NAME);
		      break;
		  }
	      }
	      if (foundsourceid == NULL) {
		  fprintf(stderr, "Could not locate requested source: %s\n", sourcename);
		  return FALSE;
	      }
	  }
	  else {
	      UInt32 source_size = source_count * sizeof(UInt32);
	      UInt32 *sourceIds = malloc(source_size);
	      if (verbose)
		  printf("Sending to every source...");
	      for (int sid=0; sid < source_count; sid++) {
		  sourceIds[sid] = airplay_sources[sid].id;
		  if (verbose)
		      printf("%s...", airplay_sources[sid].name);
	      }
	      printf("\n");
	      
	      AudioObjectPropertyAddress addr;
	      addr.mSelector = kAudioDevicePropertyDataSource;
	      addr.mScope = kAudioDevicePropertyScopeOutput;
	      addr.mElement = kAudioObjectPropertyElementMaster;
        
	      AudioObjectSetPropertyData(wantedaudev, &addr, 0, NULL, source_size, sourceIds);
	      free(sourceIds);
	  }
      }

      if (airplay_sources != NULL) {
	  free(airplay_sources);
	  airplay_sources = NULL;
      }
    }
  }

  if (wantdevname && wantedaudev == kAudioDeviceUnknown) {
    fprintf(stderr, "Could not located requested device.\n");
    return FALSE;
  }

  deviceuid = NULL;

  if (wantedaudev == kAudioDeviceUnknown) {
    if (verbose) {
      fprintf(stderr, "Using default audio device.\n");
    }
  }
  else {
    UInt32 propsize;

    propsize = sizeof(deviceuid);
    status = AudioDeviceGetProperty(wantedaudev, 0, 0,
      kAudioDevicePropertyDeviceUID, &propsize, &deviceuid);
    if (status || !deviceuid) {
      fprintf(stderr, "Could not get audio device UID.\n");
      return FALSE;
    }

    if (verbose) {
      propsize = LEN_DEVICE_NAME * sizeof(char);
      status = AudioDeviceGetProperty(wantedaudev, 1, 0,
        kAudioDevicePropertyDeviceName, &propsize, devicename);
      if (status) {
        fprintf(stderr, "Could not get audio device name.\n");
        return FALSE;
      }
      printf("Got device ID %d: \"%s\".\n", (int)wantedaudev, devicename);

    }
    if (foundsourceid != NULL) {
	AudioObjectPropertyAddress addr;

	addr.mSelector = kAudioDevicePropertyDataSource;
	addr.mScope = kAudioDevicePropertyScopeOutput;
	addr.mElement = kAudioObjectPropertyElementMaster;
	
	AudioObjectSetPropertyData(wantedaudev, &addr, 0, NULL, sizeof(UInt32), &foundsourceid);
	if (verbose) {
	    printf("Got source ID %d: \"%s\".\n", foundsourceid, sourcename);
	}
    }
  }

  if (format == -1) {
    format = sound_big_endian;
  }

  if (!ratewanted) {
    ratewanted = DEFAULT_SOUNDRATE;
  }

  rate = ratewanted;
  channels = 2;

  sound_rate = rate;
  sound_channels = channels;
  sound_format = format;
  sound_buffersize = fragsize;

  samplesperbuf = sound_buffersize / 2;
  framesperbuf = sound_buffersize / (2 * sound_channels);
    
  if (verbose) {
    printf("%d channels, %d frames per second, 16-bit samples (signed, %s)\n",
      channels, rate, (sound_format?"big-endian":"little-endian"));
    printf("%d buffers, %ld bytes (%ld frames) per buffer\n",
      bufcount, sound_buffersize, framesperbuf);
  }

  bzero(&formataq, sizeof(formataq));
  formataq.mSampleRate = sound_rate;
  formataq.mFormatID = kAudioFormatLinearPCM;
  formataq.mFramesPerPacket = 1;
  formataq.mChannelsPerFrame = sound_channels;
  formataq.mBytesPerFrame = sound_channels * 2;
  formataq.mBytesPerPacket = sound_channels * 2;
  formataq.mBitsPerChannel = 16;
  formataq.mReserved = 0;
  formataq.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger;
  if (sound_format)
    formataq.mFormatFlags |= kLinearPCMFormatFlagIsBigEndian;

  aqueue = NULL;
  status = AudioQueueNewOutput(&formataq, &playCallback, NULL, NULL, NULL, 0,
    &aqueue);
  if (status) {
    fprintf(stderr, "Unable to allocate AudioQueue.\n");
    return FALSE;
  }

  if (deviceuid) {
    status = AudioQueueSetProperty(aqueue, kAudioQueueProperty_CurrentDevice, &deviceuid, sizeof(deviceuid));
    if (status) {
      fprintf(stderr, "Unable to set requested audio device.\n");
      return FALSE;
    }
  }
  
  buffers = (buffer_t *)malloc(bufcount * sizeof(buffer_t));
  if (!buffers) {
    fprintf(stderr, "Unable to allocate buffer array\n");
    AudioQueueDispose(aqueue, TRUE);
    return FALSE;
  }

  for (ix = 0; ix < bufcount; ix++) {
    long lx;

    status = AudioQueueAllocateBuffer(aqueue, sound_buffersize, &buffers[ix].buffer);
    if (status) {
      fprintf(stderr, "Unable to allocate AudioQueueBuffer\n");
      AudioQueueDispose(aqueue, TRUE);
      return FALSE;
    }

    buffers[ix].full = FALSE;

    for (lx = 0; lx < samplesperbuf; lx++) {
      ((short*)(buffers[ix].buffer->mAudioData))[lx] = 0;
    }

    res = pthread_mutex_init(&buffers[ix].mutex, NULL);
    if (res) {
      fprintf(stderr, "Unable to init mutex.\n");
      /* free stuff */
      AudioQueueDispose(aqueue, TRUE);
      return FALSE;
    }

    res = pthread_cond_init(&buffers[ix].cond, NULL);
    if (res) {
      fprintf(stderr, "Unable to init cond.\n");
      /* free stuff */
      AudioQueueDispose(aqueue, TRUE);
      return FALSE;
    }
  }
  
  valbuffer = (long *)malloc(sizeof(long) * samplesperbuf);
  if (!valbuffer) {
    fprintf(stderr, "Unable to allocate sound buffer.\n");
    AudioQueueDispose(aqueue, TRUE);
    return FALSE;     
  }

  running = TRUE;
  started = FALSE;
  filling = 0;

  return TRUE;
}

void audev_close_device()
{
  int ix;
  long sleep;
  OSStatus status;

  if (!running) {
    fprintf(stderr, "Unable to close sound device which was never opened.\n");
    return;
  }
  
  bailing = TRUE;

  if (!started) {
    /* We never got to the point of starting playback. Do it now. */
    started = TRUE;
    status = AudioQueueStart(aqueue, NULL);
    if (status) {
      fprintf(stderr, "Could not late-start audio device.\n");
      return;
    }
  }

  status = AudioQueueFlush(aqueue);
  if (status) {
    fprintf(stderr, "Could not flush audio device; continuing.\n");
  }

  /* Wait on each buffer to make sure they're all drained. */
  for (ix = 0; ix < bufcount; ix++) {
    buffer_t *buffer = &buffers[ix];
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->full)
      pthread_cond_wait(&buffer->cond, &buffer->mutex);
    pthread_mutex_unlock(&buffer->mutex);
  }

  /* And wait one more buffer-duration. I'm not sure why this is necessary
     with all the flushing and stopping, but it is. */
  sleep = 1000 * ((1000*framesperbuf) / sound_rate); 
  usleep(sleep);

  status = AudioQueueStop(aqueue, FALSE);
  if (status) {
    fprintf(stderr, "Could not stop audio device; continuing.\n");
  }

  status = AudioQueueDispose(aqueue, FALSE);
  if (status) {
    fprintf(stderr, "Could not dispose audio device; continuing.\n");
  }

  running = FALSE;

  if (valbuffer) {
    free(valbuffer);
    valbuffer = NULL;
  }

  for (ix = 0; ix < bufcount; ix++) {
    pthread_mutex_destroy(&buffers[ix].mutex);
    pthread_cond_destroy(&buffers[ix].cond);
  }

  free(buffers);
  buffers = NULL;
}

long audev_get_soundrate()
{
  return sound_rate;
}

long audev_get_framesperbuf()
{
  return framesperbuf;
}


static void playCallback(void *user, AudioQueueRef queue, 
  AudioQueueBuffer *qbuf)
{
  int ix;
  buffer_t *buffer = NULL;

  for (ix = 0; ix < bufcount; ix++) {
    if (buffers[ix].buffer == qbuf) {
      buffer = &(buffers[ix]);
      break;
    }
  }

  if (!buffer) {
    fprintf(stderr, "Unable to identify buffer in callback.\n");
    return;
  }

  pthread_mutex_lock(&buffer->mutex);

  if (!buffer->full)
    fprintf(stderr, "Buffer %d called back but not full.\n", ix);

  buffer->full = FALSE;

  pthread_mutex_unlock(&buffer->mutex);
  pthread_cond_signal(&buffer->cond);
}

int audev_loop(mix_func_t mixfunc, generate_func_t genfunc, void *rock)
{
  char *ptr;
  int res;

  if (!running) {
    fprintf(stderr, "Sound device is not open.\n");
    return FALSE;
  }
  
  while (1) {
    OSStatus status;
    buffer_t *buffer;

    if (bailing)
      return FALSE;
    
    res = mixfunc(valbuffer, genfunc, rock);
    if (res) {
      bailing = TRUE;
      return TRUE;
    }

    buffer = &buffers[filling];

    pthread_mutex_lock(&buffer->mutex);
    
    while (buffer->full) {
      pthread_cond_wait(&buffer->cond, &buffer->mutex);
    }
    
    if (sound_format) {
      long lx;
      for (lx=0, ptr=buffer->buffer->mAudioData; lx<samplesperbuf; lx++) {
        long samp = valbuffer[lx];
        if (samp > 0x7FFF)
          samp = 0x7FFF;
        else if (samp < -0x7FFF)
          samp = -0x7FFF;
        *ptr++ = ((samp >> 8) & 0xFF);
        *ptr++ = ((samp) & 0xFF);
      }
    }
    else {
      long lx;
      for (lx=0, ptr=buffer->buffer->mAudioData; lx<samplesperbuf; lx++) {
        long samp = valbuffer[lx];
        if (samp > 0x7FFF)
          samp = 0x7FFF;
        else if (samp < -0x7FFF)
          samp = -0x7FFF;
        *ptr++ = ((samp) & 0xFF);
        *ptr++ = ((samp >> 8) & 0xFF);
      }
    }
    
    buffer->buffer->mAudioDataByteSize = sound_buffersize;
    buffer->full = TRUE;

    status = AudioQueueEnqueueBuffer(aqueue, buffer->buffer, 0, NULL);
    if (status) {
      fprintf(stderr, "Could not enqueue buffer.\n");
      return FALSE;
    }
    
    filling += 1;
    if (filling >= bufcount) 
      filling = 0;

    pthread_mutex_unlock(&buffer->mutex);

    if (!started && filling == 0) {
      /* When all the buffers are filled for the first time, we can
         start the device playback. */
      started = TRUE;
      status = AudioQueueStart(aqueue, NULL);
      if (status) {
        fprintf(stderr, "Could not start sound device.\n");
        return FALSE;
      }
    }
  }
}
