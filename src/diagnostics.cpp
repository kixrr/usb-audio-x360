// SPDX-License-Identifier: GPL-3.0-or-later
#include "diagnostics.h"

#include "playback_profile.h"
#include "capture_profile.h"
#include "storage_gate.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace {

struct KernelString {
  WORD length;
  WORD maximum_length;
  char* buffer;
};

extern "C" LONG ObCreateSymbolicLink(KernelString* link,
                                      KernelString* device);
extern "C" VOID RtlInitAnsiString(KernelString* destination,
                                   const char* source);

const DWORD kDiagnosticSchema = 1;
const DWORD kMaximumLogBytes = 256 * 1024;
const unsigned kEventSlots = 16;
const unsigned kValueCount = 20;
const unsigned kPayloadBytes = 0x400;
// One event batch is formatted here and written with a single WriteFile. The
// CRT stdio path is deliberately avoided: its per-file lock and buffered flush
// can sit on the FATX filesystem lock while the Guide is navigating.
const unsigned kWriteBufferBytes = 16 * 1024;

enum EventType {
  kEventStartup = 1,
  kEventDevice = 2,
  kEventConfiguration = 3,
  kEventSelected = 4,
  kEventFailure = 5,
  kEventStreaming = 6,
  kEventDisconnected = 7,
  kEventUi = 8,
  kEventStorage = 9,
  kEventWorkerGap = 10,
};

struct Event {
  DWORD type;
  DWORD tick;
  DWORD values[kValueCount];
  DWORD payload_length;
  BYTE payload[kPayloadBytes];
};

struct Slot {
  volatile LONG state;  // 0=free, -1=producer, 1=ready, -2=consumer.
  Event event;
};

struct TextBuffer {
  TextBuffer(char* data, size_t capacity)
      : data(data), capacity(capacity), length(0), overflow(false) {}
  void Append(const char* format, ...) {
    if (overflow || length + 1 >= capacity) {
      overflow = true;
      return;
    }
    va_list args;
    va_start(args, format);
    int written = _vsnprintf(data + length, capacity - length, format, args);
    va_end(args);
    if (written < 0) {
      data[capacity - 1] = 0;
      length = capacity - 1;
      overflow = true;
      return;
    }
    length += (size_t)written;
    if (length >= capacity) {
      length = capacity - 1;
      overflow = true;
    }
  }
  char* data;
  size_t capacity;
  size_t length;
  bool overflow;
};

static Slot g_slots[kEventSlots];
static volatile LONG g_next_slot = 0;
static volatile LONG g_dropped = 0;
static char g_log_path[MAX_PATH];
static char g_old_log_path[MAX_PATH];
static char g_write_buffer[kWriteBufferBytes];
// The log handle is kept open while the Guide is on screen, so a scene
// transition never races a create/close pair on the same volume. Outside the
// Guide it is closed after each batch (see DiagnosticsTick).
static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static bool g_initialized = false;
static bool g_module_path_resolved = false;
static const char kFallbackLogPath[] = "Usb:\\Plugins\\usb_audio360.log";
static const char kDiagnosticDrive[] = "UsbAudio360:";
static const char kDiagnosticLink[] = "\\System??\\UsbAudio360:";

struct DevicePathAlias {
  const char* device;
  const char* drive;
};

static const DevicePathAlias kDevicePathAliases[] = {
  {"\\Device\\Mass0", "Usb:"},
  {"\\Device\\Harddisk0\\Partition1", "Hdd:"},
  {"\\Device\\BuiltInMuSfc", "OnBoardMU:"},
  {"\\Device\\Mu0", "MemUnit0:"},
  {"\\Device\\Mu1", "MemUnit1:"},
};

struct XboxUnicodeString {
  WORD length;
  WORD maximum_length;
  const WCHAR* buffer;
};
typedef char RequireUnicodeStringSize8[
    sizeof(XboxUnicodeString) == 8 ? 1 : -1];

struct LoaderEntryPrefix {
  BYTE reserved[0x24];
  XboxUnicodeString full_name;
};
typedef char RequireLoaderNameAt24[
    offsetof(LoaderEntryPrefix, full_name) == 0x24 ? 1 : -1];

static void Queue(DWORD type, const DWORD* values, unsigned value_count,
                  const void* payload, DWORD payload_length) {
  if (!g_initialized) return;
  if (value_count > kValueCount) value_count = kValueCount;
  if (payload_length > kPayloadBytes) payload_length = kPayloadBytes;
  LONG start = InterlockedIncrement(&g_next_slot) - 1;
  for (unsigned attempt = 0; attempt < kEventSlots; ++attempt) {
    Slot& slot = g_slots[(start + attempt) % kEventSlots];
    if (InterlockedCompareExchange(&slot.state, -1, 0) != 0) continue;
    memset(&slot.event, 0, sizeof(slot.event));
    slot.event.type = type;
    slot.event.tick = GetTickCount();
    if (values && value_count)
      memcpy(slot.event.values, values, value_count * sizeof(DWORD));
    slot.event.payload_length = payload_length;
    if (payload && payload_length)
      memcpy(slot.event.payload, payload, payload_length);
    InterlockedExchange(&slot.state, 1);
    return;
  }
  InterlockedIncrement(&g_dropped);
}

static void CloseLog() {
  if (g_log_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_log_file);
    g_log_file = INVALID_HANDLE_VALUE;
  }
}

static HANDLE OpenAppend() {
  if (g_log_file != INVALID_HANDLE_VALUE) return g_log_file;
  g_log_file = CreateFileA(g_log_path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (g_log_file != INVALID_HANDLE_VALUE)
    SetFilePointer(g_log_file, 0, 0, FILE_END);
  return g_log_file;
}

static void RotateIfNeeded() {
  if (StorageIsUiActive()) return;
  HANDLE existing = OpenAppend();
  if (existing == INVALID_HANDLE_VALUE) return;
  DWORD high = 0;
  DWORD size = GetFileSize(existing, &high);
  if (high || size < kMaximumLogBytes) return;
  CloseLog();
  DeleteFileA(g_old_log_path);
  MoveFileA(g_log_path, g_old_log_path);
}

static bool SetLogPath(const char* path) {
  if (!path || strlen(path) + 5 > sizeof(g_old_log_path)) return false;
  CloseLog();
  strcpy(g_log_path, path);
  strcpy(g_old_log_path, path);
  strcat(g_old_log_path, ".old");
  return true;
}

static const char* DecisionName(DWORD decision) {
  switch (decision) {
    case kDeviceRejectedProfile: return "rejected-profile";
    case kDeviceSlotBusy: return "slot-busy";
    case kDeviceTransportRejected: return "transport-rejected";
    case kDeviceClaimed: return "claimed";
    default: return "unknown";
  }
}

static const char* RejectionName(DWORD rejection) {
  switch (rejection) {
    case 0: return "none";
    case 1: return "invalid-handle";
    case 2: return "missing-interface";
    case 3: return "nonzero-alternate";
    case 4: return "not-audio";
    case 5: return "not-audio-streaming";
    case 6: return "unsupported-uac-protocol";
    case 7: return "missing-controller";
    case 8: return "not-full-speed-ohci";
    default: return "unknown";
  }
}

static const char* StageName(DWORD stage) {
  static const char* names[] = {
    "header", "descriptor", "configuration", "inactive", "clock",
    "uac1-rate", "unmute", "active", "finished", "opening",
    "read-mute", "read-volume", "verify-interface", "verify-clock",
    "verify-mute", "verify-volume", "uac1-fallback-inactive",
    "uac1-fallback-rate", "capture-active", "capture-rate",
    "verify-capture"
  };
  return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "unknown";
}

static void WriteFormat(TextBuffer& out, size_t index, const uac::Format& f) {
  out.Append(
      "  format[%u] uac=%u cfg=%u ac_if=%u as_if=%u alt=%u channels=%u "
      "bytes=%u valid_bits=%u direction=%s terminal=%u clock=%u sync=%u "
      "data=0x%02x/%u/interval%u feedback=0x%02x/%u/interval%u "
      "rate_known=%u rate_48000=%u fixed_48000=%u rate_control=%u "
      "layout=%u topology=%u playback_supported=%u capture_supported=%u\n",
      (unsigned)index, f.version, f.configuration, f.control_interface,
      f.interface_number, f.alternate, f.channels, f.sample_bytes,
      f.valid_bits, f.direction == uac::kCapture ? "capture" : "playback",
      f.terminal, f.clock, f.sync, f.data.address,
      f.data.max_packet_bytes, f.data.interval, f.feedback.address,
      f.feedback.max_packet_bytes, f.feedback.interval,
      f.rate_48000_known ? 1 : 0, f.supports_48000 ? 1 : 0,
      f.fixed_48000 ? 1 : 0, f.endpoint_rate_control ? 1 : 0,
      f.endpoint_layout_supported ? 1 : 0, f.topology_valid ? 1 : 0,
      uac::SupportedFullSpeedPlayback(f) ? 1 : 0,
      uac::SupportedFullSpeedCapture(f) ? 1 : 0);
}

static void WriteConfiguration(TextBuffer& out, const Event& event) {
  uac::Format formats[32];
  size_t count = 0;
  uac::Result result = uac::Discover(event.payload, event.payload_length,
                                     formats, 32, &count);
  out.Append("tick=%lu event=configuration bytes=%lu discover=%u formats=%u\n",
             event.tick, event.payload_length, result, (unsigned)count);
  for (size_t index = 0; index < count; ++index)
    WriteFormat(out, index, formats[index]);
  for (DWORD offset = 0; offset < event.payload_length; offset += 16) {
    out.Append("  descriptor %04lx:", offset);
    DWORD end = offset + 16;
    if (end > event.payload_length) end = event.payload_length;
    for (DWORD index = offset; index < end; ++index)
      out.Append(" %02x", event.payload[index]);
    out.Append("\n");
  }
}

static void WriteEvent(TextBuffer& out, const Event& event) {
  const DWORD* v = event.values;
  switch (event.type) {
    case kEventStartup:
      out.Append(
          "\n=== USB Audio 360 diagnostics schema=%lu kernel=%lu tick=%lu ===\n",
          v[0], v[1], event.tick);
      break;
    case kEventDevice:
      out.Append(
          "tick=%lu event=device decision=%s add_status=0x%08lx "
          "vid=%04lx pid=%04lx usb=%04lx device=%04lx "
          "device_class=%02lx/%02lx/%02lx configurations=%lu "
          "interface=%lu alt=%lu class=%02lx/%02lx/%02lx endpoints=%lu "
          "controller=%lu rejection=%lu(%s)\n",
          event.tick, DecisionName(v[0]), v[1], v[2], v[3], v[4], v[5],
          v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14],
          v[15], v[16], v[17], RejectionName(v[17]));
      break;
    case kEventConfiguration:
      WriteConfiguration(out, event);
      break;
    case kEventSelected:
      out.Append(
          "tick=%lu event=selected uac=%lu cfg=%lu ac_if=%lu as_if=%lu "
          "alt=%lu data=0x%02lx/%lu feedback=0x%02lx/%lu "
          "bytes=%lu valid_bits=%lu sync=%lu clock=%lu feature=%lu\n",
          event.tick, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
          v[8], v[9], v[10], v[11], v[12], v[13]);
      break;
    case kEventFailure:
      out.Append(
          "tick=%lu event=failure error=0x%08lx stage=%lu(%s) "
          "request=0x%08lx index=0x%04lx status=0x%08lx received=%lu "
          "clock_error=%lu uac=%lu as_if=%lu alt=%lu data=0x%02lx/%lu "
          "feedback=0x%02lx/%lu\n",
          event.tick, v[0], v[1], StageName(v[1]), v[2], v[3], v[4], v[5],
          v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13]);
      break;
    case kEventStreaming:
      out.Append("tick=%lu event=streaming\n", event.tick);
      break;
    case kEventDisconnected:
      out.Append("tick=%lu event=disconnected\n", event.tick);
      break;
    case kEventUi:
      out.Append("tick=%lu event=ui tag=%s a=0x%08lx b=0x%08lx\n",
                 event.tick, (const char*)event.payload, v[0], v[1]);
      break;
    case kEventStorage:
      out.Append("tick=%lu event=storage tag=%s a=%lu b=%lu\n",
                 event.tick, (const char*)event.payload, v[0], v[1]);
      break;
    case kEventWorkerGap:
      out.Append("tick=%lu event=worker-gap gap_ms=%lu\n", event.tick, v[0]);
      break;
  }
}

}  // namespace

void DiagnosticsInitialize(HANDLE module, WORD kernel_build) {
  memset(g_slots, 0, sizeof(g_slots));
  SetLogPath(kFallbackLogPath);
  g_initialized = true;
  const LoaderEntryPrefix* entry = (const LoaderEntryPrefix*)module;
  if (entry && entry->full_name.buffer && entry->full_name.length &&
      !(entry->full_name.length & 1) &&
      entry->full_name.length <=
          (sizeof(g_log_path) - 1) * sizeof(WCHAR)) {
    char loaded_path[MAX_PATH];
    memset(loaded_path, 0, sizeof(loaded_path));
    DWORD length = entry->full_name.length / sizeof(WCHAR);
    bool ascii = true;
    for (DWORD index = 0; index < length; ++index) {
      WCHAR value = entry->full_name.buffer[index];
      if (value > 0x7f) {
        ascii = false;
        break;
      }
      loaded_path[index] = (char)value;
    }
    if (ascii) {
      loaded_path[length] = 0;
      const char* source = loaded_path;
      const char* prefix = 0;
      char device_root[MAX_PATH];
      memset(device_root, 0, sizeof(device_root));
      for (unsigned index = 0;
           index < sizeof(kDevicePathAliases) / sizeof(kDevicePathAliases[0]);
           ++index) {
        size_t device_length = strlen(kDevicePathAliases[index].device);
        if (strncmp(loaded_path, kDevicePathAliases[index].device,
                    device_length) == 0 &&
            (loaded_path[device_length] == '\\' ||
             loaded_path[device_length] == '/' ||
             loaded_path[device_length] == 0)) {
          if (device_length + 2 <= sizeof(device_root)) {
            memcpy(device_root, loaded_path, device_length);
            device_root[device_length] = '\\';
            KernelString link;
            KernelString device;
            RtlInitAnsiString(&link, kDiagnosticLink);
            RtlInitAnsiString(&device, device_root);
            ObCreateSymbolicLink(&link, &device);
            prefix = kDiagnosticDrive;
          } else {
            prefix = kDevicePathAliases[index].drive;
          }
          source = loaded_path + device_length;
          break;
        }
      }
      char resolved[MAX_PATH];
      memset(resolved, 0, sizeof(resolved));
      size_t prefix_length = prefix ? strlen(prefix) : 0;
      if (prefix_length + strlen(source) + 1 <= sizeof(resolved)) {
        if (prefix) strcpy(resolved, prefix);
        strcat(resolved, source);
        char* separator = 0;
        for (char* cursor = resolved; *cursor; ++cursor)
          if (*cursor == '\\' || *cursor == '/') separator = cursor;
        const char log_name[] = "usb_audio360.log";
        if (separator) {
          separator[1] = 0;
          if (strlen(resolved) + sizeof(log_name) <= sizeof(resolved)) {
            strcat(resolved, log_name);
            SetLogPath(resolved);
            g_module_path_resolved = true;
          }
        }
      }
    }
  }
  DWORD values[2] = {kDiagnosticSchema, kernel_build};
  Queue(kEventStartup, values, 2, 0, 0);
}

bool DiagnosticsSettingsPath(char* output, DWORD capacity) {
  if (!output || !g_module_path_resolved) return false;
  size_t length = strlen(g_log_path);
  // .ini has the same length as .log; reserve suffix room for .bak/.tmp.
  if (length < 4 || length + 5 > capacity) return false;
  memcpy(output, g_log_path, length + 1);
  strcpy(output + length - 4, ".ini");
  return true;
}

void DiagnosticsObserveDevice(const UsbAudioDeviceObservation& o) {
  DWORD values[kValueCount] = {
    o.decision, (DWORD)o.add_status, o.vendor_id, o.product_id,
    o.usb_version, o.device_version, o.device_class, o.device_subclass,
    o.device_protocol, o.configuration_count, o.interface_number,
    o.alternate_setting, o.interface_class, o.interface_subclass,
    o.interface_protocol, o.endpoint_count, o.controller, o.rejection, 0, 0
  };
  Queue(kEventDevice, values, kValueCount, 0, 0);
}

void DiagnosticsConfiguration(const BYTE* descriptor, DWORD length) {
  Queue(kEventConfiguration, 0, 0, descriptor, length);
}

void DiagnosticsSelected(const AudioProfile& p, const uac::Format& f) {
  DWORD values[14] = {
    p.audio_class_version, p.configuration, p.control_interface_number,
    p.interface_number, p.alternate_setting, p.endpoint_address,
    p.endpoint_packet_size, p.feedback_endpoint_address,
    p.feedback_endpoint_packet_size, p.bytes_per_sample, f.valid_bits,
    f.sync, p.clock_source_id, p.feature_unit_id
  };
  Queue(kEventSelected, values, 14, 0, 0);
}

void DiagnosticsFailure(DWORD error, DWORD stage, DWORD request,
                        DWORD index, LONG status, DWORD received,
                        DWORD clock_error, const AudioProfile& p,
                        const uac::Format& f) {
  DWORD values[14] = {
    error, stage, request, index, (DWORD)status, received, clock_error,
    p.audio_class_version, p.interface_number, p.alternate_setting,
    p.endpoint_address, p.endpoint_packet_size, p.feedback_endpoint_address,
    f.feedback.max_packet_bytes
  };
  Queue(kEventFailure, values, 14, 0, 0);
}

void DiagnosticsStreaming() { Queue(kEventStreaming, 0, 0, 0, 0); }
void DiagnosticsDisconnected() { Queue(kEventDisconnected, 0, 0, 0, 0); }

void DiagnosticsUi(const char* tag, DWORD a, DWORD b) {
  if (!tag) return;
  DWORD values[2] = {a, b};
  Queue(kEventUi, values, 2, tag, (DWORD)strlen(tag) + 1);
}

void DiagnosticsStorage(const char* tag, DWORD a, DWORD b) {
  if (!tag) return;
  DWORD values[2] = {a, b};
  Queue(kEventStorage, values, 2, tag, (DWORD)strlen(tag) + 1);
}

void DiagnosticsWorkerGap(DWORD gap_ms) {
  DWORD values[1] = {gap_ms};
  Queue(kEventWorkerGap, values, 1, 0, 0);
}

void DiagnosticsTick() {
  if (!g_initialized) return;
  bool pending = g_dropped != 0;
  for (unsigned index = 0; !pending && index < kEventSlots; ++index)
    pending = g_slots[index].state == 1;
  if (!pending) return;
  // Never wait behind the settings save: the notification thread also feeds
  // audio notifications and Guide installation, so it must stay live.
  if (!StorageTryLock()) return;
  DWORD started = GetTickCount();
  RotateIfNeeded();
  HANDLE file = OpenAppend();
  if (file == INVALID_HANDLE_VALUE &&
      strcmp(g_log_path, kFallbackLogPath) != 0) {
    SetLogPath(kFallbackLogPath);
    RotateIfNeeded();
    file = OpenAppend();
  }
  if (file == INVALID_HANDLE_VALUE) {
    StorageUnlock();
    return;
  }
  TextBuffer out(g_write_buffer, sizeof(g_write_buffer));
  LONG dropped = InterlockedExchange(&g_dropped, 0);
  if (dropped)
    out.Append("tick=%lu event=queue-overflow dropped=%ld\n",
               GetTickCount(), dropped);
  unsigned consumed[kEventSlots];
  unsigned consumed_count = 0;
  for (unsigned index = 0; index < kEventSlots; ++index) {
    Slot& slot = g_slots[index];
    if (InterlockedCompareExchange(&slot.state, -2, 1) != 1) continue;
    size_t before = out.length;
    WriteEvent(out, slot.event);
    if (out.overflow) {
      // Do not lose an event to the buffer limit: put the slot back and let the
      // next tick continue from here.
      out.length = before;
      out.overflow = false;
      break;
    }
    consumed[consumed_count++] = index;
  }
  DWORD written = 0;
  bool wrote = true;
  if (out.length) {
    SetFilePointer(file, 0, 0, FILE_END);
    wrote = WriteFile(file, g_write_buffer, (DWORD)out.length, &written, 0) != FALSE;
  }
  if (wrote) {
    for (unsigned i = 0; i < consumed_count; ++i)
      InterlockedExchange(&g_slots[consumed[i]].state, 0);
  } else {
    // The batch stays queued and is retried; a failed write must not silently
    // delete the evidence we are collecting.
    for (unsigned i = 0; i < consumed_count; ++i)
      InterlockedExchange(&g_slots[consumed[i]].state, 1);
    CloseLog();
  }
  // While the Guide owns the screen the handle stays open: no create/close
  // traffic in the exact window a scene transition needs the volume. Outside
  // the Guide the handle is closed after each batch so the directory entry
  // (and therefore xbdm/FTP readers) sees the new size immediately.
  if (!StorageIsUiActive()) CloseLog();
  StorageUnlock();
  DWORD elapsed = GetTickCount() - started;
  if (elapsed >= 1000u || !wrote)
    DiagnosticsStorage("log-write", elapsed, wrote ? written : 0xffffffffu);
}
