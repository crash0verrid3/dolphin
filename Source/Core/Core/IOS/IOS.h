// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/IOSC.h"

class PointerWrap;

namespace DiscIO
{
class CNANDContentLoader;
}

namespace IOS
{
namespace HLE
{
namespace Device
{
class Device;
}

struct Request;
struct OpenRequest;

struct IPCCommandResult
{
  s32 return_value;
  bool send_reply;
  u64 reply_delay_ticks;
};

enum IPCCommandType : u32
{
  IPC_CMD_OPEN = 1,
  IPC_CMD_CLOSE = 2,
  IPC_CMD_READ = 3,
  IPC_CMD_WRITE = 4,
  IPC_CMD_SEEK = 5,
  IPC_CMD_IOCTL = 6,
  IPC_CMD_IOCTLV = 7,
  // This is used for replies to commands.
  IPC_REPLY = 8,
};

enum ProcessId : u32
{
  PID_KERNEL = 0,
  PID_ES = 1,
  PID_FS = 2,
  PID_DI = 3,
  PID_OH0 = 4,
  PID_OH1 = 5,
  PID_EHCI = 6,
  PID_SDI = 7,
  PID_USBETH = 8,
  PID_NET = 9,
  PID_WD = 10,
  PID_WL = 11,
  PID_KD = 12,
  PID_NCD = 13,
  PID_STM = 14,
  PID_PPCBOOT = 15,
  PID_SSL = 16,
  PID_USB = 17,
  PID_P2P = 18,
  PID_UNKNOWN = 19,
};

// HLE for the IOS kernel: IPC, device management, syscalls, and Dolphin-specific, IOS-wide calls.
class Kernel
{
public:
  explicit Kernel(u64 ios_title_id);
  ~Kernel();

  void DoState(PointerWrap& p);
  void HandleIPCEvent(u64 userdata);
  void UpdateIPC();
  void UpdateDevices();
  void UpdateWantDeterminism(bool new_want_determinism);

  std::shared_ptr<Device::Device> GetDeviceByName(const std::string& device_name);
  void SDIO_EventNotify();

  void EnqueueIPCRequest(u32 address);
  void EnqueueIPCReply(const Request& request, s32 return_value, int cycles_in_future = 0,
                       CoreTiming::FromThread from = CoreTiming::FromThread::CPU);

  void SetUidForPPC(u32 uid);
  u32 GetUidForPPC() const;
  void SetGidForPPC(u16 gid);
  u16 GetGidForPPC() const;

  bool BootstrapPPC(const DiscIO::CNANDContentLoader& content_loader);
  bool BootIOS(u64 ios_title_id);
  u32 GetVersion() const;

  IOSC& GetIOSC();

private:
  void ExecuteIPCCommand(u32 address);
  IPCCommandResult HandleIPCCommand(const Request& request);
  void EnqueueIPCAcknowledgement(u32 address, int cycles_in_future = 0);

  void AddDevice(std::unique_ptr<Device::Device> device);
  void AddStaticDevices();
  s32 GetFreeDeviceID();
  s32 OpenDevice(OpenRequest& request);

  u64 m_title_id = 0;
  static constexpr u8 IPC_MAX_FDS = 0x18;
  std::map<std::string, std::shared_ptr<Device::Device>> m_device_map;
  std::mutex m_device_map_mutex;
  // TODO: make this fdmap per process.
  std::array<std::shared_ptr<Device::Device>, IPC_MAX_FDS> m_fdmap;

  u32 m_ppc_uid = 0;
  u16 m_ppc_gid = 0;

  using IPCMsgQueue = std::deque<u32>;
  IPCMsgQueue m_request_queue;  // ppc -> arm
  IPCMsgQueue m_reply_queue;    // arm -> ppc
  IPCMsgQueue m_ack_queue;      // arm -> ppc
  u64 m_last_reply_time = 0;

  IOSC m_iosc;
};

// Used for controlling and accessing an IOS instance that is tied to emulation.
void Init();
void Shutdown();
Kernel* GetIOS();

}  // namespace HLE
}  // namespace IOS
