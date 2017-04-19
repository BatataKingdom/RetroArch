/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>

#ifdef _WIN32_WINNT 
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <Winerror.h>
#include <initguid.h>
#include <Mmdeviceapi.h>
#include <Mmreg.h>
#include <Audioclient.h>
#include <Propidl.h>

#include <lists/string_list.h>

#include "../audio_driver.h"
#include "../../verbosity.h"
#include "../../configuration.h"

DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14); /* DEVPROP_TYPE_STRING */


#define WASAPI_WARN(bool_exp, err_str, warn_exp)\
      if (!(bool_exp)) {\
         wasapi_warn(err_str);\
         warn_exp; }

#define WASAPI_CHECK(bool_exp, err_str, err_exp)\
      if (!(bool_exp)) {\
         wasapi_err(err_str);\
         err_exp; }

#define WASAPI_HR_CHECK(hr, fun_str, err_exp)\
      if (FAILED(hr)) {\
         wasapi_com_err(fun_str, hr);\
         err_exp; }

#define WASAPI_HR_WARN(hr, fun_str, warn_exp)\
      if (FAILED(hr)) {\
         wasapi_com_warn(fun_str, hr);\
         warn_exp; }

#define WASAPI_SR_CHECK(bool_exp, fun_str, err_exp)\
      if (!(bool_exp)) {\
         wasapi_sys_err(fun_str);\
         err_exp; }

#define WASAPI_RELEASE(iface)\
      if(iface) {\
         iface->lpVtbl->Release(iface);\
         iface = NULL; }\

#define WASAPI_FREE(ptr)\
      if(ptr) {\
         free(ptr);\
         ptr = NULL; }\

#define WASAPI_CO_FREE(ptr)\
      if(ptr) {\
         CoTaskMemFree(ptr);\
         ptr = NULL; }\

typedef struct
{
   IMMDevice *device;
   IAudioClient *client;
   IAudioRenderClient *renderer;
   HANDLE write_event;
   void *buffer;        /* NULL in shared mode */
   size_t buffer_size;  /* in shared mode holds WASAPI engine buffer size */
   size_t buffer_usage; /* valid in exclusive mode only */
   size_t frame_size;   /* 4 or 8 only */
   bool blocking;
   bool running;
} wasapi_t;

static void wasapi_log(const char *msg)
{
   RARCH_LOG("[WASAPI]: %s.\n", msg);
}

static void wasapi_warn(const char *warn)
{
   RARCH_WARN("[WASAPI]: %s.\n", warn);
}

static void wasapi_err(const char *err)
{
   RARCH_ERR("[WASAPI]: %s.\n", err);
}

static void wasapi_com_warn(const char *fun, HRESULT hr)
{
   RARCH_WARN("[WASAPI]: %s failed with error 0x%.8X.\n", fun, hr);
}

static void wasapi_com_err(const char *fun, HRESULT hr)
{
   RARCH_ERR("[WASAPI]: %s failed with error 0x%.8X.\n", fun, hr);
}

static void wasapi_sys_err(const char *fun)
{
   RARCH_ERR("[WASAPI]: %s failed with error %d.\n", fun, GetLastError());
}

static bool wasapi_check_device_id(IMMDevice *device, const char *id)
{
   HRESULT hr;
   bool result   = false;
   LPWSTR dev_id = NULL, dev_cmp_id = NULL;
   int id_length = MultiByteToWideChar(CP_ACP, 0, id, -1, NULL, 0);

   WASAPI_SR_CHECK(id_length > 0, "MultiByteToWideChar", goto error);

   dev_cmp_id = (LPWSTR)malloc(id_length * sizeof(WCHAR));
   WASAPI_CHECK(dev_cmp_id, "Out of memory", goto error);

   id_length = MultiByteToWideChar(CP_ACP, 0, id, -1, dev_cmp_id, id_length);
   WASAPI_SR_CHECK(id_length > 0, "MultiByteToWideChar", goto error);
   
   hr = device->lpVtbl->GetId(device, &dev_id);
   WASAPI_HR_CHECK(hr, "IMMDevice::GetId", goto error);

   result = lstrcmpW(dev_cmp_id, dev_id) == 0 ? true : false;

   WASAPI_CO_FREE(dev_id);
   WASAPI_FREE(dev_cmp_id);

   return result;

error:
   WASAPI_CO_FREE(dev_id);
   WASAPI_FREE(dev_cmp_id);

   return false;
}

static IMMDevice *wasapi_init_device(const char *id)
{
   HRESULT hr;
   UINT32 dev_count, i;
   IMMDeviceEnumerator *enumerator = NULL;
   IMMDevice *device               = NULL;
   IMMDeviceCollection *collection = NULL;

   if (id)
      RARCH_LOG("[WASAPI]: Initializing device %s ...\n", id);
   else
      wasapi_log("Initializing default device ..");

   hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
         &IID_IMMDeviceEnumerator, (void **)&enumerator);
   WASAPI_HR_CHECK(hr, "CoCreateInstance", goto error);

   if (id)
   {
      hr = enumerator->lpVtbl->EnumAudioEndpoints(enumerator,
         eRender, DEVICE_STATE_ACTIVE, &collection);
      WASAPI_HR_CHECK(hr, "IMMDeviceEnumerator::EnumAudioEndpoints",
            goto error);

      hr = collection->lpVtbl->GetCount(collection, &dev_count);
      WASAPI_HR_CHECK(hr, "IMMDeviceCollection::GetCount", goto error);

      for (i = 0; i < dev_count; ++i)
      {
         hr = collection->lpVtbl->Item(collection, i, &device);
         WASAPI_HR_CHECK(hr, "IMMDeviceCollection::Item", continue);

         if (wasapi_check_device_id(device, id))
            break;

         WASAPI_RELEASE(device);
      }
   }
   else
   {
      hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator,
            eRender, eConsole, &device);
      WASAPI_HR_CHECK(hr, "IMMDeviceEnumerator::GetDefaultAudioEndpoint",
            goto error);
   }

   if (!device)
      goto error;

   WASAPI_RELEASE(collection);
   WASAPI_RELEASE(enumerator);

   wasapi_log("Device initialized");

   return device;

error:
   WASAPI_RELEASE(collection);
   WASAPI_RELEASE(enumerator);

   if (id)
      wasapi_warn("Failed to initialize device");
   else
      wasapi_err("Failed to initialize device");

   return NULL;
}

static unsigned wasapi_pref_rate(unsigned i)
{
   const unsigned r[] = { 48000, 44100, 96000, 192000 };

   if (i >= sizeof(r) / sizeof(unsigned))
      return 0;

   return r[i];
}

static void wasapi_set_format(WAVEFORMATEXTENSIBLE *wf,
      bool float_fmt, unsigned rate)
{
   wf->Format.nChannels               = 2;
   wf->Format.nSamplesPerSec          = rate;

   if (float_fmt)
   {
      wf->Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
      wf->Format.nAvgBytesPerSec      = rate * 8;
      wf->Format.nBlockAlign          = 8;
      wf->Format.wBitsPerSample       = 32;
      wf->Format.cbSize               = sizeof(WORD) + sizeof(DWORD) + sizeof(GUID);
      wf->Samples.wValidBitsPerSample = 32;
      wf->dwChannelMask               = KSAUDIO_SPEAKER_STEREO;
      wf->SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
   }
   else
   {
      wf->Format.wFormatTag           = WAVE_FORMAT_PCM;
      wf->Format.nAvgBytesPerSec      = rate * 4;
      wf->Format.nBlockAlign          = 4;
      wf->Format.wBitsPerSample       = 16;
      wf->Format.cbSize               = 0;
   }
}

static IAudioClient *wasapi_init_client_sh(IMMDevice *device,
      bool *float_fmt, unsigned *rate, double *latency)
{
   HRESULT hr;
   WAVEFORMATEXTENSIBLE wf;
   int i, j;
   IAudioClient *client          = NULL;
   bool float_fmt_res            = *float_fmt;
   unsigned rate_res             = *rate;
   REFERENCE_TIME default_period = 0;
   REFERENCE_TIME stream_latency = 0;

   hr = device->lpVtbl->Activate(device, &IID_IAudioClient,
         CLSCTX_ALL, NULL, (void**)&client);
   WASAPI_HR_CHECK(hr, "IMMDevice::Activate", return NULL);

   /* once for float, once for pcm (requested first) */
   for (i = 0; i < 2; ++i)
   {
      rate_res = *rate;
      if (i == 1)
         float_fmt_res = !float_fmt_res;

      /* for requested rate (first) and all preferred rates */
      for (j = 0; rate_res; ++j)
      {
         RARCH_LOG("[WASAPI]: Initializing client (shared, %s, %uHz) ...\n",
               float_fmt_res ? "float" : "pcm", rate_res);

         wasapi_set_format(&wf, float_fmt_res, rate_res);
         hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED,
               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
               0, 0, (WAVEFORMATEX*)&wf, NULL);
         if (hr == AUDCLNT_E_ALREADY_INITIALIZED)
         {
            client->lpVtbl->Release(client);
            hr = device->lpVtbl->Activate(device, &IID_IAudioClient,
               CLSCTX_ALL, NULL, (void**)&client);
            WASAPI_HR_CHECK(hr, "IMMDevice::Activate", return NULL);

            hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  0, 0, (WAVEFORMATEX*)&wf, NULL);
         }
         if (hr != AUDCLNT_E_UNSUPPORTED_FORMAT)
         {
            i = 2; /* break from outer loop too */
            break;
         }

         wasapi_warn("Unsupported format");
         rate_res = wasapi_pref_rate(j);
         if (rate_res == *rate) /* requested allready tested */
            rate_res = wasapi_pref_rate(++j); /* skip it */
      }
   }

   WASAPI_HR_CHECK(hr, "IAudioClient::Initialize", goto error);

   *float_fmt = float_fmt_res;
   *rate = rate_res;
   *latency = 0.0;

   /* next two calls are allowed to fail (we losing latency info only) */
   hr = client->lpVtbl->GetStreamLatency(client, &stream_latency);
   WASAPI_HR_WARN(hr, "IAudioClient::GetStreamLatency", return client);

   hr = client->lpVtbl->GetDevicePeriod(client, &default_period, NULL);
   WASAPI_HR_WARN(hr, "IAudioClient::GetDevicePeriod", return client);

   *latency = (double)(stream_latency + default_period) / 10000.0;

   return client;

error:
   WASAPI_RELEASE(client);

   return NULL;
}

static IAudioClient *wasapi_init_client_ex(IMMDevice *device,
      bool *float_fmt, unsigned *rate, double *latency)
{
   HRESULT hr;
   WAVEFORMATEXTENSIBLE wf;
   int i, j;
   IAudioClient *client           = NULL;
   bool float_fmt_res             = *float_fmt;
   unsigned rate_res              = *rate;
   REFERENCE_TIME default_period  = 0;
   REFERENCE_TIME minimum_period  = 0;
   REFERENCE_TIME buffer_duration = 0;
   REFERENCE_TIME stream_latency  = 0;
   UINT32 buffer_length           = 0;

   hr = device->lpVtbl->Activate(device, &IID_IAudioClient,
         CLSCTX_ALL, NULL, (void**)&client);
   WASAPI_HR_CHECK(hr, "IMMDevice::Activate", return NULL);

   hr = client->lpVtbl->GetDevicePeriod(client, &default_period, &minimum_period);
   WASAPI_HR_CHECK(hr, "IAudioClient::GetDevicePeriod", goto error);

   /* buffer_duration is 2/3 of requested latency (in 100ns units) */
   buffer_duration = *latency * 10000.0 * 2.0 / 3.0;
   if (buffer_duration < minimum_period)
      buffer_duration = minimum_period;

   /* once for float, once for pcm (requested first) */
   for (i = 0; i < 2; ++i)
   {
      rate_res = *rate;
      if (i == 1)
         float_fmt_res = !float_fmt_res;

      /* for requested rate (first) and all preferred rates */
      for (j = 0; rate_res; ++j)
      {
         RARCH_LOG("[WASAPI]: Initializing client "
               "(exclusive, %s, %uHz, %.1fms) ...\n",
               float_fmt_res ? "float" : "pcm", rate_res, *latency);

         wasapi_set_format(&wf, float_fmt_res, rate_res);
         hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
               buffer_duration, buffer_duration, (WAVEFORMATEX*)&wf, NULL);
         if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
         {
            hr = client->lpVtbl->GetBufferSize(client, &buffer_length);
            WASAPI_HR_CHECK(hr, "IAudioClient::GetBufferSize", goto error);

            client->lpVtbl->Release(client);
            hr = device->lpVtbl->Activate(device, &IID_IAudioClient,
               CLSCTX_ALL, NULL, (void**)&client);
            WASAPI_HR_CHECK(hr, "IMMDevice::Activate", return NULL);

            buffer_duration = 10000.0 * 1000.0 / rate_res * buffer_length + 0.5;
            hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_EXCLUSIVE,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  buffer_duration, buffer_duration, (WAVEFORMATEX*)&wf, NULL);
         }
         if (hr == AUDCLNT_E_ALREADY_INITIALIZED)
         {
            client->lpVtbl->Release(client);
            hr = device->lpVtbl->Activate(device, &IID_IAudioClient,
               CLSCTX_ALL, NULL, (void**)&client);
            WASAPI_HR_CHECK(hr, "IMMDevice::Activate", return NULL);

            hr = client->lpVtbl->Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                  0, 0, (WAVEFORMATEX*)&wf, NULL);
         }
         if (hr != AUDCLNT_E_UNSUPPORTED_FORMAT)
         {
            WASAPI_WARN(hr != AUDCLNT_E_DEVICE_IN_USE,
                  "Device already in use", goto error);

            WASAPI_WARN(hr != AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED,
                  "Exclusive mode disabled", goto error);

            i = 2; /* break from outer loop too */
            break;
         }

         wasapi_warn("Unsupported format");
         rate_res = wasapi_pref_rate(j);
         if (rate_res == *rate) /* requested allready tested */
            rate_res = wasapi_pref_rate(++j); /* skip it */
      }
   }

   WASAPI_HR_CHECK(hr, "IAudioClient::Initialize", goto error);

   *float_fmt = float_fmt_res;
   *rate = rate_res;
   *latency = 0.0;

   /* next call is allowed to fail (we losing latency info only) */
   hr = client->lpVtbl->GetStreamLatency(client, &stream_latency);
   WASAPI_HR_WARN(hr, "IAudioClient::GetStreamLatency", return client);

   *latency = (double)stream_latency / 10000.0 * 1.5;

   return client;

error:
   WASAPI_RELEASE(client);

   return NULL;
}

static IAudioClient *wasapi_init_client(IMMDevice *device, bool *exclusive,
      bool *float_fmt, unsigned *rate, unsigned latency)
{
   HRESULT hr;
   double latency_res       = latency;
   IAudioClient *client     = NULL;

   if (*exclusive)
   {
      client = wasapi_init_client_ex(device, float_fmt, rate, &latency_res);
      if (!client)
      {
         client = wasapi_init_client_sh(device, float_fmt, rate, &latency_res);
         if (client)
            *exclusive = false;
      }
   }
   else
   {
      client = wasapi_init_client_sh(device, float_fmt, rate, &latency_res);
      if (!client)
      {
         client = wasapi_init_client_ex(device, float_fmt, rate, &latency_res);
         if (client)
            *exclusive = true;
      }
   }

   WASAPI_CHECK(client, "Failed to initialize client", return NULL);

   RARCH_LOG("[WASAPI]: Client initialized (%s, %s, %uHz, %.1fms).\n",
         *exclusive ? "exclusive" : "shared",
         *float_fmt ? "float" : "pcm",
         *rate, latency_res);

   return client;
}

static void *wasapi_init(const char *dev_id, unsigned rate, unsigned latency,
      unsigned u, unsigned *new_rate)
{
   HRESULT hr;
   bool com_initialized = false;
   UINT32 frame_count   = 0;
   BYTE *dest           = NULL;
   settings_t *settings = config_get_ptr();
   bool exclusive       = settings->audio.wasapi.exclusive_mode;
   bool float_format    = settings->audio.wasapi.float_format;
   wasapi_t *w          = (wasapi_t*)calloc(1, sizeof(wasapi_t));

   WASAPI_CHECK(w, "Out of memory", return NULL);

   hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
   WASAPI_HR_CHECK(hr, "CoInitializeEx", goto error);

   com_initialized = true;

   w->device = wasapi_init_device(dev_id);
   if (!w->device && dev_id)
      w->device = wasapi_init_device(NULL);
   if (!w->device)
      goto error;

   *new_rate = rate;
   w->client = wasapi_init_client(w->device,
         &exclusive, &float_format, new_rate, latency);
   if (!w->client)
      goto error;

   hr = w->client->lpVtbl->GetBufferSize(w->client, &frame_count);
   WASAPI_HR_CHECK(hr, "IAudioClient::GetBufferSize", goto error);

   w->frame_size = float_format ? 8 : 4;
   w->buffer_size = frame_count * w->frame_size;
   if (exclusive)
   {
      w->buffer = malloc(w->buffer_size);
      WASAPI_CHECK(w->buffer, "Out of memory", goto error);
   }

   w->write_event = CreateEventA(NULL, FALSE, FALSE, NULL);
   WASAPI_SR_CHECK(w->write_event, "CreateEventA", goto error);

   hr = w->client->lpVtbl->SetEventHandle(w->client, w->write_event);
   WASAPI_HR_CHECK(hr, "IAudioClient::SetEventHandle", goto error);

   hr = w->client->lpVtbl->GetService(w->client,
         &IID_IAudioRenderClient, (void**)&w->renderer);
   WASAPI_HR_CHECK(hr, "IAudioClient::GetService", goto error);

   hr = w->renderer->lpVtbl->GetBuffer(w->renderer, frame_count, &dest);
   WASAPI_HR_CHECK(hr, "IAudioRenderClient::GetBuffer", goto error);

   hr = w->renderer->lpVtbl->ReleaseBuffer(w->renderer, frame_count,
         AUDCLNT_BUFFERFLAGS_SILENT);
   WASAPI_HR_CHECK(hr, "IAudioRenderClient::ReleaseBuffer", goto error);

   return w;

error:
   WASAPI_RELEASE(w->renderer);
   WASAPI_RELEASE(w->client);      
   WASAPI_RELEASE(w->device);
   if (w->write_event)
      CloseHandle(w->write_event);
   WASAPI_FREE(w->buffer);
   free(w);
   if (com_initialized)
      CoUninitialize();

   return NULL;
}

static bool wasapi_flush(wasapi_t * w, const void * data, size_t size)
{
   BYTE *dest         = NULL;
   UINT32 frame_count = size / w->frame_size;
   HRESULT hr         = w->renderer->lpVtbl->GetBuffer(w->renderer, frame_count, &dest);

   WASAPI_HR_CHECK(hr, "IAudioRenderClient::GetBuffer", return false)

   memcpy(dest, data, size);
   hr = w->renderer->lpVtbl->ReleaseBuffer(w->renderer, frame_count, 0);

   WASAPI_HR_CHECK(hr, "IAudioRenderClient::ReleaseBuffer", return false);

   return true;
}

static ssize_t wasapi_write_sh(wasapi_t *w, const void * data, size_t size)
{
   DWORD ir;
   size_t buffer_avail;
   HRESULT hr;
   bool br;
   ssize_t result;
   UINT32 padding = 0;

   if (w->blocking)
   {
      ir = WaitForSingleObject(w->write_event, INFINITE);
      WASAPI_SR_CHECK(ir == WAIT_OBJECT_0, "WaitForSingleObject", return -1);
   }

   hr = w->client->lpVtbl->GetCurrentPadding(w->client, &padding);
   WASAPI_HR_CHECK(hr, "IAudioClient::GetCurrentPadding", return -1);

   buffer_avail = w->buffer_size - padding * w->frame_size;
   if (!buffer_avail)
      return 0;

   result = size > buffer_avail ? buffer_avail : size;
   br = wasapi_flush(w, data, result);

   return br ? result : -1;
}

static ssize_t wasapi_write_ex(wasapi_t *w, const void * data, size_t size)
{
   DWORD ir;
   size_t buffer_avail;
   bool br        = false;
   ssize_t result = 0;

   if (w->buffer_usage == w->buffer_size)
   {
      ir = WaitForSingleObject(w->write_event, w->blocking ? INFINITE : 0);
      if (ir != WAIT_OBJECT_0 && w->blocking)
      {
         wasapi_sys_err("WaitForSingleObject");
         return -1;
      }
      if (ir != WAIT_OBJECT_0)
         return 0;

      br = wasapi_flush(w, w->buffer, w->buffer_size);
      if (!br)
         return -1;

      w->buffer_usage = 0;
   }

   buffer_avail = w->buffer_size - w->buffer_usage;
   result = size < buffer_avail ? size : buffer_avail;
   memcpy(w->buffer + w->buffer_usage, data, result);
   w->buffer_usage += result;

   return result;
}

static ssize_t wasapi_write(void *wh, const void *data, size_t size, bool u)
{
   size_t writen;
   ssize_t ir;
   wasapi_t *w = (wasapi_t*)wh;

   if (w->blocking)
   {
      for (writen = 0, ir = -1; writen < size && ir; writen += ir)
      {
         if (w->buffer)
            ir = wasapi_write_ex(w, data + writen, size - writen);
         else
            ir = wasapi_write_sh(w, data + writen, size - writen);
         if (ir == -1)
         {
            writen = -1;
            break;
         }
      }
   }
   else if (w->buffer)
      writen = wasapi_write_ex(w, data, size);
   else
      writen = wasapi_write_sh(w, data, size);

   return writen;
}

static bool wasapi_stop(void *wh)
{
   wasapi_t *w = (wasapi_t*)wh;
   HRESULT hr  = w->client->lpVtbl->Stop(w->client);

   WASAPI_HR_CHECK(hr, "IAudioClient::Stop", return !w->running);

   w->running = false;

   return true;
}

static bool wasapi_start(void *wh, bool u)
{
   wasapi_t *w = (wasapi_t*)wh;
   HRESULT hr  = w->client->lpVtbl->Start(w->client);

   WASAPI_HR_CHECK(hr, "IAudioClient::Start", return w->running);

   w->running = true;

   return true;
}

static bool wasapi_alive(void *wh)
{
   wasapi_t *w = (wasapi_t*)wh;

   return w->running;
}

static void wasapi_set_nonblock_state(void *wh, bool nonblock)
{
   wasapi_t *w = (wasapi_t*)wh;

   RARCH_LOG("[WASAPI]: Sync %s.\n", nonblock ? "off" : "on");

   w->blocking = !nonblock;
}

static void wasapi_free(void *wh)
{
   wasapi_t *w        = (wasapi_t*)wh;
   HANDLE write_event = w->write_event;
   DWORD ir;

   WASAPI_RELEASE(w->renderer);
   if (w->client)
      w->client->lpVtbl->Stop(w->client);
   WASAPI_RELEASE(w->client);
   WASAPI_RELEASE(w->device);
   CoUninitialize();
   WASAPI_FREE(w->buffer);
   free(w);

   ir = WaitForSingleObject(write_event, 20);
   if (ir == WAIT_FAILED)
      wasapi_sys_err("WaitForSingleObject");

   /* If event isn't signaled log and leak */
   WASAPI_CHECK(ir == WAIT_OBJECT_0, "Memory leak in wasapi_free", return);

   CloseHandle(write_event);
}

static bool wasapi_use_float(void *wh)
{
   wasapi_t *w = (wasapi_t*)wh;

   return w->frame_size == 8;
}

static void *wasapi_device_list_new(void *u)
{
   HRESULT hr;
   UINT i;
   PROPVARIANT prop_var;
   int ir;
   union string_list_elem_attr attr;
   IMMDeviceEnumerator *enumerator = NULL;
   IMMDeviceCollection *collection = NULL;
   UINT dev_count                  = 0;
   IMMDevice *device               = NULL;
   LPWSTR dev_id_wstr              = NULL;
   LPWSTR dev_name_wstr            = NULL;
   IPropertyStore *prop_store      = NULL;
   bool prop_var_init              = false;
   bool br                         = false;
   char *dev_id_str                = NULL;
   char *dev_name_str              = NULL;
   struct string_list *sl          = string_list_new();

   wasapi_log("Enumerating active devices ...");

   WASAPI_CHECK(sl, "string_list_new failed", return NULL);

   attr.i = 0;
   hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
         &IID_IMMDeviceEnumerator, (void **)&enumerator);
   WASAPI_HR_CHECK(hr, "CoCreateInstance", goto error);

   hr = enumerator->lpVtbl->EnumAudioEndpoints(enumerator,
         eRender, DEVICE_STATE_ACTIVE, &collection);
   WASAPI_HR_CHECK(hr, "IMMDeviceEnumerator::EnumAudioEndpoints", goto error);

   hr = collection->lpVtbl->GetCount(collection, &dev_count);
   WASAPI_HR_CHECK(hr, "IMMDeviceCollection::GetCount", goto error);

   for (i = 0; i < dev_count; ++i)
   {
      hr = collection->lpVtbl->Item(collection, i, &device);
      WASAPI_HR_CHECK(hr, "IMMDeviceCollection::Item", goto error);

      hr = device->lpVtbl->GetId(device, &dev_id_wstr);
      WASAPI_HR_CHECK(hr, "IMMDevice::GetId", goto error);

      ir = WideCharToMultiByte(CP_ACP, 0, dev_id_wstr, -1,
            NULL, 0, NULL, NULL);
      WASAPI_SR_CHECK(ir, "WideCharToMultiByte", goto error);

      dev_id_str = (char *)malloc(ir);
      WASAPI_CHECK(dev_id_str, "Out of memory", goto error);

      ir = WideCharToMultiByte(CP_ACP, 0, dev_id_wstr, -1,
            dev_id_str, ir, NULL, NULL);
      WASAPI_SR_CHECK(ir, "WideCharToMultiByte", goto error);

      hr = device->lpVtbl->OpenPropertyStore(device, STGM_READ, &prop_store);
      WASAPI_HR_CHECK(hr, "IMMDevice::OpenPropertyStore", goto error);

      PropVariantInit(&prop_var);
      prop_var_init = true;
      hr = prop_store->lpVtbl->GetValue(prop_store,
            &PKEY_Device_FriendlyName, &prop_var);
      WASAPI_HR_CHECK(hr, "IPropertyStore::GetValue", goto error);

      ir = WideCharToMultiByte(CP_ACP, 0, prop_var.pwszVal, -1,
            NULL, 0, NULL, NULL);
      WASAPI_SR_CHECK(ir, "WideCharToMultiByte", goto error);

      dev_name_str = (char *)malloc(ir);
      WASAPI_CHECK(dev_name_str, "Out of memory", goto error);

      ir = WideCharToMultiByte(CP_ACP, 0, prop_var.pwszVal, -1,
            dev_name_str, ir, NULL, NULL);
      WASAPI_SR_CHECK(ir, "WideCharToMultiByte", goto error);

      RARCH_LOG("[WASAPI]: %s %s\n", dev_name_str, dev_id_str);

      br = string_list_append(sl, dev_id_str, attr);
      WASAPI_CHECK(br, "string_list_append failed", goto error);

      PropVariantClear(&prop_var);
      prop_var_init = false;
      WASAPI_CO_FREE(dev_id_wstr);
      WASAPI_CO_FREE(dev_name_wstr);
      WASAPI_FREE(dev_id_str);
      WASAPI_FREE(dev_name_str);
      WASAPI_RELEASE(prop_store);
      WASAPI_RELEASE(device);
   }

   WASAPI_RELEASE(collection);
   WASAPI_RELEASE(enumerator);

   wasapi_log("Devices enumerated");

   return sl;

error:
   WASAPI_FREE(dev_id_str);
   WASAPI_FREE(dev_name_str);
   if (prop_var_init)
      PropVariantClear(&prop_var);
   WASAPI_RELEASE(prop_store);
   WASAPI_CO_FREE(dev_id_wstr);
   WASAPI_CO_FREE(dev_name_wstr);
   WASAPI_RELEASE(device);
   WASAPI_RELEASE(collection);
   WASAPI_RELEASE(enumerator);
   if (sl)
      string_list_free(sl);

   wasapi_err("Device enumeration failed");

   return NULL;
}

static void wasapi_device_list_free(void *u, void *slp)
{
   struct string_list *sl = (struct string_list*)slp;

   string_list_free(sl);
}

static size_t wasapi_write_avail(void *wh)
{
   HRESULT hr;
   wasapi_t *w    = (wasapi_t*)wh;
   UINT32 padding = 0;

   if (w->buffer)
      return w->buffer_size - w->buffer_usage;

   hr = w->client->lpVtbl->GetCurrentPadding(w->client, &padding);
   WASAPI_HR_CHECK(hr, "IAudioClient::GetCurrentPadding", return 0);
   
   return w->buffer_size - padding * w->frame_size;
}

static size_t wasapi_buffer_size(void *wh)
{
   wasapi_t *w = (wasapi_t*)wh;

   return w->buffer_size;
}

audio_driver_t audio_wasapi = {
   wasapi_init,
   wasapi_write,
   wasapi_stop,
   wasapi_start,
   wasapi_alive,
   wasapi_set_nonblock_state,
   wasapi_free,
   wasapi_use_float,
   "wasapi",
   wasapi_device_list_new,
   wasapi_device_list_free,
   wasapi_write_avail,
   wasapi_buffer_size
};