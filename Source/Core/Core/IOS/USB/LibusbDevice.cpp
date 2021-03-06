// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <utility>

#include <libusb.h>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/LibusbDevice.h"

namespace IOS
{
namespace HLE
{
namespace USB
{
LibusbDevice::LibusbDevice(Kernel& ios, libusb_device* device,
                           const libusb_device_descriptor& descriptor)
    : m_ios(ios), m_device(device)
{
  libusb_ref_device(m_device);
  m_vid = descriptor.idVendor;
  m_pid = descriptor.idProduct;
  m_id = (static_cast<u64>(m_vid) << 32 | static_cast<u64>(m_pid) << 16 |
          static_cast<u64>(libusb_get_bus_number(device)) << 8 |
          static_cast<u64>(libusb_get_device_address(device)));

  for (u8 i = 0; i < descriptor.bNumConfigurations; ++i)
    m_config_descriptors.emplace_back(std::make_unique<LibusbConfigDescriptor>(m_device, i));
}

LibusbDevice::~LibusbDevice()
{
  if (m_device_attached)
    DetachInterface();
  if (m_handle != nullptr)
    libusb_close(m_handle);
  libusb_unref_device(m_device);
}

DeviceDescriptor LibusbDevice::GetDeviceDescriptor() const
{
  libusb_device_descriptor device_descriptor;
  libusb_get_device_descriptor(m_device, &device_descriptor);
  DeviceDescriptor descriptor;
  // The libusb_device_descriptor struct is the same as the IOS one, and it's not going to change.
  std::memcpy(&descriptor, &device_descriptor, sizeof(descriptor));
  return descriptor;
}

std::vector<ConfigDescriptor> LibusbDevice::GetConfigurations() const
{
  std::vector<ConfigDescriptor> descriptors;
  for (const auto& config_descriptor : m_config_descriptors)
  {
    if (!config_descriptor->IsValid())
    {
      ERROR_LOG(IOS_USB, "Ignoring invalid config descriptor for %04x:%04x", m_vid, m_pid);
      continue;
    }
    ConfigDescriptor descriptor;
    std::memcpy(&descriptor, config_descriptor->Get(), sizeof(descriptor));
    descriptors.push_back(descriptor);
  }
  return descriptors;
}

std::vector<InterfaceDescriptor> LibusbDevice::GetInterfaces(const u8 config) const
{
  std::vector<InterfaceDescriptor> descriptors;
  if (config >= m_config_descriptors.size() || !m_config_descriptors[config]->IsValid())
  {
    ERROR_LOG(IOS_USB, "Invalid config descriptor %u for %04x:%04x", config, m_vid, m_pid);
    return descriptors;
  }
  for (u8 i = 0; i < m_config_descriptors[config]->Get()->bNumInterfaces; ++i)
  {
    const libusb_interface& interface = m_config_descriptors[config]->Get()->interface[i];
    for (u8 a = 0; a < interface.num_altsetting; ++a)
    {
      InterfaceDescriptor descriptor;
      std::memcpy(&descriptor, &interface.altsetting[a], sizeof(descriptor));
      descriptors.push_back(descriptor);
    }
  }
  return descriptors;
}

std::vector<EndpointDescriptor>
LibusbDevice::GetEndpoints(const u8 config, const u8 interface_number, const u8 alt_setting) const
{
  std::vector<EndpointDescriptor> descriptors;
  if (config >= m_config_descriptors.size() || !m_config_descriptors[config]->IsValid())
  {
    ERROR_LOG(IOS_USB, "Invalid config descriptor %u for %04x:%04x", config, m_vid, m_pid);
    return descriptors;
  }
  _assert_(interface_number < m_config_descriptors[config]->Get()->bNumInterfaces);
  const auto& interface = m_config_descriptors[config]->Get()->interface[interface_number];
  _assert_(alt_setting < interface.num_altsetting);
  const libusb_interface_descriptor& interface_descriptor = interface.altsetting[alt_setting];
  for (u8 i = 0; i < interface_descriptor.bNumEndpoints; ++i)
  {
    EndpointDescriptor descriptor;
    std::memcpy(&descriptor, &interface_descriptor.endpoint[i], sizeof(descriptor));
    descriptors.push_back(descriptor);
  }
  return descriptors;
}

std::string LibusbDevice::GetErrorName(const int error_code) const
{
  return libusb_error_name(error_code);
}

bool LibusbDevice::Attach(const u8 interface)
{
  if (m_device_attached && interface != m_active_interface)
    return ChangeInterface(interface) == 0;

  if (m_device_attached)
    return true;

  m_device_attached = false;
  NOTICE_LOG(IOS_USB, "[%04x:%04x] Opening device", m_vid, m_pid);
  const int ret = libusb_open(m_device, &m_handle);
  if (ret != 0)
  {
    ERROR_LOG(IOS_USB, "[%04x:%04x] Failed to open: %s", m_vid, m_pid, libusb_error_name(ret));
    return false;
  }
  if (AttachInterface(interface) != 0)
    return false;
  m_device_attached = true;
  return true;
}

int LibusbDevice::CancelTransfer(const u8 endpoint)
{
  INFO_LOG(IOS_USB, "[%04x:%04x %d] Cancelling transfers (endpoint 0x%x)", m_vid, m_pid,
           m_active_interface, endpoint);
  const auto iterator = m_transfer_endpoints.find(endpoint);
  if (iterator == m_transfer_endpoints.cend())
    return IPC_ENOENT;
  iterator->second.CancelTransfers();
  return IPC_SUCCESS;
}

int LibusbDevice::ChangeInterface(const u8 interface)
{
  if (!m_device_attached || interface >= m_config_descriptors[0]->Get()->bNumInterfaces)
    return LIBUSB_ERROR_NOT_FOUND;

  INFO_LOG(IOS_USB, "[%04x:%04x %d] Changing interface to %d", m_vid, m_pid, m_active_interface,
           interface);
  const int ret = DetachInterface();
  if (ret < 0)
    return ret;
  return AttachInterface(interface);
}

int LibusbDevice::SetAltSetting(const u8 alt_setting)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  INFO_LOG(IOS_USB, "[%04x:%04x %d] Setting alt setting %d", m_vid, m_pid, m_active_interface,
           alt_setting);
  return libusb_set_interface_alt_setting(m_handle, m_active_interface, alt_setting);
}

int LibusbDevice::SubmitTransfer(std::unique_ptr<CtrlMessage> cmd)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  switch ((cmd->request_type << 8) | cmd->request)
  {
  // The following requests have to go through libusb and cannot be directly sent to the device.
  case USBHDR(DIR_HOST2DEVICE, TYPE_STANDARD, REC_INTERFACE, REQUEST_SET_INTERFACE):
  {
    if (static_cast<u8>(cmd->index) != m_active_interface)
    {
      const int ret = ChangeInterface(static_cast<u8>(cmd->index));
      if (ret < 0)
      {
        ERROR_LOG(IOS_USB, "[%04x:%04x %d] Failed to change interface to %d: %s", m_vid, m_pid,
                  m_active_interface, cmd->index, libusb_error_name(ret));
        return ret;
      }
    }
    const int ret = SetAltSetting(static_cast<u8>(cmd->value));
    if (ret == 0)
      m_ios.EnqueueIPCReply(cmd->ios_request, cmd->length);
    return ret;
  }
  case USBHDR(DIR_HOST2DEVICE, TYPE_STANDARD, REC_DEVICE, REQUEST_SET_CONFIGURATION):
  {
    const int ret = libusb_set_configuration(m_handle, cmd->value);
    if (ret == 0)
      m_ios.EnqueueIPCReply(cmd->ios_request, cmd->length);
    return ret;
  }
  }

  const size_t size = cmd->length + LIBUSB_CONTROL_SETUP_SIZE;
  auto buffer = std::make_unique<u8[]>(size);
  libusb_fill_control_setup(buffer.get(), cmd->request_type, cmd->request, cmd->value, cmd->index,
                            cmd->length);
  Memory::CopyFromEmu(buffer.get() + LIBUSB_CONTROL_SETUP_SIZE, cmd->data_address, cmd->length);
  libusb_transfer* transfer = libusb_alloc_transfer(0);
  transfer->flags |= LIBUSB_TRANSFER_FREE_TRANSFER;
  libusb_fill_control_transfer(transfer, m_handle, buffer.release(), CtrlTransferCallback, this, 0);
  m_transfer_endpoints[0].AddTransfer(std::move(cmd), transfer);
  return libusb_submit_transfer(transfer);
}

int LibusbDevice::SubmitTransfer(std::unique_ptr<BulkMessage> cmd)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  libusb_transfer* transfer = libusb_alloc_transfer(0);
  libusb_fill_bulk_transfer(transfer, m_handle, cmd->endpoint,
                            cmd->MakeBuffer(cmd->length).release(), cmd->length, TransferCallback,
                            this, 0);
  transfer->flags |= LIBUSB_TRANSFER_FREE_TRANSFER;
  m_transfer_endpoints[transfer->endpoint].AddTransfer(std::move(cmd), transfer);
  return libusb_submit_transfer(transfer);
}

int LibusbDevice::SubmitTransfer(std::unique_ptr<IntrMessage> cmd)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  libusb_transfer* transfer = libusb_alloc_transfer(0);
  libusb_fill_interrupt_transfer(transfer, m_handle, cmd->endpoint,
                                 cmd->MakeBuffer(cmd->length).release(), cmd->length,
                                 TransferCallback, this, 0);
  transfer->flags |= LIBUSB_TRANSFER_FREE_TRANSFER;
  m_transfer_endpoints[transfer->endpoint].AddTransfer(std::move(cmd), transfer);
  return libusb_submit_transfer(transfer);
}

int LibusbDevice::SubmitTransfer(std::unique_ptr<IsoMessage> cmd)
{
  if (!m_device_attached)
    return LIBUSB_ERROR_NOT_FOUND;

  libusb_transfer* transfer = libusb_alloc_transfer(cmd->num_packets);
  transfer->buffer = cmd->MakeBuffer(cmd->length).release();
  transfer->callback = TransferCallback;
  transfer->dev_handle = m_handle;
  transfer->endpoint = cmd->endpoint;
  transfer->flags |= LIBUSB_TRANSFER_FREE_TRANSFER;
  for (size_t i = 0; i < cmd->num_packets; ++i)
    transfer->iso_packet_desc[i].length = cmd->packet_sizes[i];
  transfer->length = cmd->length;
  transfer->num_iso_packets = cmd->num_packets;
  transfer->timeout = 0;
  transfer->type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
  transfer->user_data = this;
  m_transfer_endpoints[transfer->endpoint].AddTransfer(std::move(cmd), transfer);
  return libusb_submit_transfer(transfer);
}

void LibusbDevice::CtrlTransferCallback(libusb_transfer* transfer)
{
  auto* device = static_cast<LibusbDevice*>(transfer->user_data);
  device->m_transfer_endpoints[0].HandleTransfer(transfer, [&](const auto& cmd) {
    cmd.FillBuffer(libusb_control_transfer_get_data(transfer), transfer->actual_length);
    // The return code is the total transfer length -- *including* the setup packet.
    return transfer->length;
  });
}

void LibusbDevice::TransferCallback(libusb_transfer* transfer)
{
  auto* device = static_cast<LibusbDevice*>(transfer->user_data);
  device->m_transfer_endpoints[transfer->endpoint].HandleTransfer(transfer, [&](const auto& cmd) {
    switch (transfer->type)
    {
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
    {
      auto& iso_msg = static_cast<const IsoMessage&>(cmd);
      cmd.FillBuffer(transfer->buffer, iso_msg.length);
      for (size_t i = 0; i < iso_msg.num_packets; ++i)
        iso_msg.SetPacketReturnValue(i, transfer->iso_packet_desc[i].actual_length);
      // Note: isochronous transfers *must* return 0 as the return value. Anything else
      // (such as the number of bytes transferred) is considered as a failure.
      return static_cast<s32>(IPC_SUCCESS);
    }
    default:
      cmd.FillBuffer(transfer->buffer, transfer->actual_length);
      return static_cast<s32>(transfer->actual_length);
    }
  });
}

static const std::map<u8, const char*> s_transfer_types = {
    {LIBUSB_TRANSFER_TYPE_CONTROL, "Control"},
    {LIBUSB_TRANSFER_TYPE_ISOCHRONOUS, "Isochronous"},
    {LIBUSB_TRANSFER_TYPE_BULK, "Bulk"},
    {LIBUSB_TRANSFER_TYPE_INTERRUPT, "Interrupt"},
};

void LibusbDevice::TransferEndpoint::AddTransfer(std::unique_ptr<TransferCommand> command,
                                                 libusb_transfer* transfer)
{
  std::lock_guard<std::mutex> lk{m_transfers_mutex};
  m_transfers.emplace(transfer, std::move(command));
}

void LibusbDevice::TransferEndpoint::HandleTransfer(libusb_transfer* transfer,
                                                    std::function<s32(const TransferCommand&)> fn)
{
  std::lock_guard<std::mutex> lk{m_transfers_mutex};
  const auto iterator = m_transfers.find(transfer);
  if (iterator == m_transfers.cend())
  {
    ERROR_LOG(IOS_USB, "No such transfer");
    return;
  }

  const std::unique_ptr<u8[]> buffer(transfer->buffer);
  const auto& cmd = *iterator->second.get();
  const auto* device = static_cast<LibusbDevice*>(transfer->user_data);
  s32 return_value = 0;
  switch (transfer->status)
  {
  case LIBUSB_TRANSFER_COMPLETED:
    return_value = fn(cmd);
    break;
  case LIBUSB_TRANSFER_ERROR:
  case LIBUSB_TRANSFER_CANCELLED:
  case LIBUSB_TRANSFER_TIMED_OUT:
  case LIBUSB_TRANSFER_OVERFLOW:
  case LIBUSB_TRANSFER_STALL:
    ERROR_LOG(IOS_USB, "[%04x:%04x %d] %s transfer (endpoint 0x%02x) failed: %s", device->m_vid,
              device->m_pid, device->m_active_interface, s_transfer_types.at(transfer->type),
              transfer->endpoint, libusb_error_name(transfer->status));
    return_value = transfer->status == LIBUSB_TRANSFER_STALL ? -7004 : -5;
    break;
  case LIBUSB_TRANSFER_NO_DEVICE:
    return_value = IPC_ENOENT;
    break;
  }
  cmd.OnTransferComplete(return_value);
  m_transfers.erase(transfer);
}

void LibusbDevice::TransferEndpoint::CancelTransfers()
{
  std::lock_guard<std::mutex> lk(m_transfers_mutex);
  if (m_transfers.empty())
    return;
  INFO_LOG(IOS_USB, "Cancelling %ld transfer(s)", m_transfers.size());
  for (const auto& pending_transfer : m_transfers)
    libusb_cancel_transfer(pending_transfer.first);
}

int LibusbDevice::GetNumberOfAltSettings(const u8 interface_number)
{
  return m_config_descriptors[0]->Get()->interface[interface_number].num_altsetting;
}

int LibusbDevice::AttachInterface(const u8 interface)
{
  if (m_handle == nullptr)
  {
    ERROR_LOG(IOS_USB, "[%04x:%04x] Cannot attach without a valid device handle", m_vid, m_pid);
    return -1;
  }

  INFO_LOG(IOS_USB, "[%04x:%04x] Attaching interface %d", m_vid, m_pid, interface);
  const int ret = libusb_detach_kernel_driver(m_handle, interface);
  if (ret < 0 && ret != LIBUSB_ERROR_NOT_FOUND && ret != LIBUSB_ERROR_NOT_SUPPORTED)
  {
    ERROR_LOG(IOS_USB, "[%04x:%04x] Failed to detach kernel driver: %s", m_vid, m_pid,
              libusb_error_name(ret));
    return ret;
  }
  const int r = libusb_claim_interface(m_handle, interface);
  if (r < 0)
  {
    ERROR_LOG(IOS_USB, "[%04x:%04x] Couldn't claim interface %d: %s", m_vid, m_pid, interface,
              libusb_error_name(r));
    return r;
  }
  m_active_interface = interface;
  return 0;
}

int LibusbDevice::DetachInterface()
{
  if (m_handle == nullptr)
  {
    ERROR_LOG(IOS_USB, "[%04x:%04x] Cannot detach without a valid device handle", m_vid, m_pid);
    return -1;
  }

  INFO_LOG(IOS_USB, "[%04x:%04x] Detaching interface %d", m_vid, m_pid, m_active_interface);
  const int ret = libusb_release_interface(m_handle, m_active_interface);
  if (ret < 0 && ret != LIBUSB_ERROR_NO_DEVICE)
  {
    ERROR_LOG(IOS_USB, "[%04x:%04x] Failed to release interface %d: %s", m_vid, m_pid,
              m_active_interface, libusb_error_name(ret));
    return ret;
  }
  return 0;
}

LibusbConfigDescriptor::LibusbConfigDescriptor(libusb_device* device, const u8 config_num)
{
  if (libusb_get_config_descriptor(device, config_num, &m_descriptor) != LIBUSB_SUCCESS)
    m_descriptor = nullptr;
}

LibusbConfigDescriptor::~LibusbConfigDescriptor()
{
  if (m_descriptor != nullptr)
    libusb_free_config_descriptor(m_descriptor);
}
}  // namespace USB
}  // namespace HLE
}  // namespace IOS
