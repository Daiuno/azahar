// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <vector>
#include <boost/serialization/shared_ptr.hpp>
#include "common/alignment.h"
#include "common/archives.h"
#include "common/logging/log.h"
#include "common/memory_ref.h"
#include "core/core.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/ipc.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/memory.h"

SERIALIZE_EXPORT_IMPL(Kernel::MappedBufferContext)

namespace Kernel {

Result TranslateCommandBuffer(Kernel::KernelSystem& kernel, Memory::MemorySystem& memory,
                              std::shared_ptr<Thread> src_thread,
                              std::shared_ptr<Thread> dst_thread, VAddr src_address,
                              VAddr dst_address,
                              std::vector<MappedBufferContext>& mapped_buffer_context, bool reply) {
    auto src_process = src_thread->owner_process.lock();
    auto dst_process = dst_thread->owner_process.lock();
    ASSERT(src_process && dst_process);

    // Do NOT clear mapped_buffer_context here on new requests: doing so drops live mappings
    // without UnmapRange, leaks VM state, and can leave the next ReplyAndReceive unable to match
    // buffers (freeze / spurious errors). Stale entries should be rare if each request is paired
    // with a reply; if they appear, fix the session/reply ordering rather than clearing blindly.

    IPC::Header header;
    // TODO(Subv): Replace by Memory::Read32 when possible.
    memory.ReadBlock(*src_process, src_address, &header.raw, sizeof(header.raw));

    std::size_t untranslated_size = 1u + header.normal_params_size;
    std::size_t command_size = untranslated_size + header.translate_params_size;

    // Header fields allow up to 127 words; TLS still stores command + static descriptors in one
    // contiguous region (command may extend past 0x100 bytes into the static-buffer slots). Using
    // a fixed 64-word buffer mis-decodes translate descriptors and breaks LLE IPC.
    constexpr std::size_t MaxIpcCommandWords = 128;
    if (command_size > MaxIpcCommandWords) {
        LOG_ERROR(Kernel, "IPC command size ({}) is invalid (max {})", command_size,
                  MaxIpcCommandWords);
        return Result(ErrCodes::CommandTooLarge, ErrorModule::OS, ErrorSummary::InvalidState,
                      ErrorLevel::Status);
    }

    if (command_size > IPC::COMMAND_BUFFER_LENGTH) {
        LOG_DEBUG(Kernel,
                  "IPC command size ({}) exceeds 0x100-byte command buffer ({} words); "
                  "reading full translate region (real TLS layout)",
                  command_size, IPC::COMMAND_BUFFER_LENGTH);
    }

    std::vector<u32> cmd_buf(command_size);
    memory.ReadBlock(*src_process, src_address, cmd_buf.data(), command_size * sizeof(u32));

    const bool should_record = kernel.GetIPCRecorder().IsEnabled();

    std::vector<u32> untranslated_cmdbuf;
    if (should_record) {
        untranslated_cmdbuf = std::vector<u32>{cmd_buf.begin(), cmd_buf.begin() + command_size};
    }

    std::size_t i = untranslated_size;
    while (i < command_size) {
        u32 descriptor = cmd_buf[i];
        i += 1;

        switch (IPC::GetDescriptorType(descriptor)) {
        case IPC::DescriptorType::CopyHandle:
        case IPC::DescriptorType::MoveHandle: {
            u32 num_handles = IPC::HandleNumberFromDesc(descriptor);
            // Note: The real kernel does not check that the number of handles fits into the command
            // buffer before writing them, only after finishing.
            if (i + num_handles > command_size) {
                return Result(ErrCodes::CommandTooLarge, ErrorModule::OS,
                              ErrorSummary::InvalidState, ErrorLevel::Status);
            }

            for (u32 j = 0; j < num_handles; ++j) {
                Handle handle = cmd_buf[i];
                std::shared_ptr<Object> object = nullptr;
                // Perform pseudo-handle detection here because by the time this function is called,
                // the current thread and process are no longer the ones which created this IPC
                // request, but the ones that are handling it.
                if (handle == CurrentThread) {
                    object = src_thread;
                } else if (handle == CurrentProcess) {
                    object = src_process;
                } else if (handle != 0) {
                    object = src_process->handle_table.GetGeneric(handle);
                    if (descriptor == IPC::DescriptorType::MoveHandle) {
                        src_process->handle_table.Close(handle);
                    }
                }

                if (object == nullptr) {
                    // Note: The real kernel sets invalid translated handles to 0 in the target
                    // command buffer.
                    cmd_buf[i++] = 0;
                    continue;
                }

                R_ASSERT(dst_process->handle_table.Create(std::addressof(cmd_buf[i++]),
                                                          std::move(object)));
            }
            break;
        }
        case IPC::DescriptorType::CallingPid: {
            cmd_buf[i++] = src_process->process_id;
            break;
        }
        case IPC::DescriptorType::StaticBuffer: {
            IPC::StaticBufferDescInfo bufferInfo{descriptor};
            VAddr static_buffer_src_address = cmd_buf[i];

            // Grab the address that the target thread set up to receive the response static buffer
            // and write our data there. The static buffers area is located right after the command
            // buffer area.
            struct StaticBuffer {
                IPC::StaticBufferDescInfo descriptor;
                VAddr address;
            };

            static_assert(sizeof(StaticBuffer) == 8, "StaticBuffer struct has incorrect size.");

            StaticBuffer target_buffer;

            u32 static_buffer_offset = IPC::COMMAND_BUFFER_LENGTH * sizeof(u32) +
                                       sizeof(StaticBuffer) * bufferInfo.buffer_id;
            memory.ReadBlock(*dst_process, dst_address + static_buffer_offset, &target_buffer,
                             sizeof(target_buffer));

            const u32 src_bytes = bufferInfo.size;
            const u32 dst_bytes = target_buffer.descriptor.size;
            const u32 transfer_bytes = std::min(src_bytes, dst_bytes);

            if (src_bytes > dst_bytes) {
                LOG_WARNING(Kernel,
                            "Static buffer IPC truncated: src_size={} dst_size={} buffer_id={} "
                            "(sender payload larger than receiver slot; clamping)",
                            src_bytes, dst_bytes, static_cast<u32>(bufferInfo.buffer_id));
            }

            std::vector<u8> data(transfer_bytes);
            if (transfer_bytes > 0) {
                memory.ReadBlock(*src_process, static_buffer_src_address, data.data(), data.size());
                memory.WriteBlock(*dst_process, target_buffer.address, data.data(), data.size());
            }

            cmd_buf[i++] = target_buffer.address;
            break;
        }
        case IPC::DescriptorType::MappedBuffer: {
            IPC::MappedBufferDescInfo descInfo{descriptor};
            VAddr source_address = cmd_buf[i];

            u32 size = static_cast<u32>(descInfo.size);
            IPC::MappedBufferPermissions permissions = descInfo.perms;

            VAddr page_start = Common::AlignDown(source_address, Memory::CITRA_PAGE_SIZE);
            u32 page_offset = source_address - page_start;
            u32 num_pages = Common::AlignUp(page_offset + size, Memory::CITRA_PAGE_SIZE) >>
                            Memory::CITRA_PAGE_BITS;

            // Skip when the size is zero and num_pages == 0
            if (size == 0) {
                cmd_buf[i++] = 0;
                break;
            }
            ASSERT(num_pages >= 1);

            if (reply) {
                // Scan the target's command buffer for the matching mapped buffer.
                // The real kernel panics if you try to reply with an unsolicited MappedBuffer.
                auto found = std::find_if(
                    mapped_buffer_context.begin(), mapped_buffer_context.end(),
                    [permissions, size, source_address](const MappedBufferContext& context) {
                        // Note: reply's source_address is request's target_address
                        return context.permissions == permissions && context.size == size &&
                               context.target_address == source_address;
                    });
                if (found == mapped_buffer_context.end()) {
                    // LLE services sometimes reply with a slightly different size/perm mask than
                    // the original request used when registering the buffer; match by IPC address.
                    found = std::find_if(
                        mapped_buffer_context.begin(), mapped_buffer_context.end(),
                        [source_address](const MappedBufferContext& context) {
                            return context.target_address == source_address;
                        });
                    if (found != mapped_buffer_context.end()) {
                        LOG_WARNING(Kernel,
                                    "MappedBuffer reply relaxed match @ {:#010x}: "
                                    "ctx size={} perm={} vs desc size={} perm={}",
                                    source_address, found->size,
                                    static_cast<u32>(found->permissions), size,
                                    static_cast<u32>(permissions));
                    }
                }
                if (found == mapped_buffer_context.end()) {
                    LOG_ERROR(Kernel,
                              "MappedBuffer reply: no context for addr={:#010x} perm={} size={} "
                              "(remaining contexts={})",
                              source_address, static_cast<u32>(permissions), size,
                              mapped_buffer_context.size());
                    return ResultInvalidBufferDescriptor;
                }

                const u32 copy_size = std::min(size, found->size);

                if (permissions != IPC::MappedBufferPermissions::R) {
                    // Copy the modified buffer back into the target process
                    // NOTE: As this is a reply the "source" is the destination and the
                    //       "target" is the source.
                    memory.CopyBlock(*dst_process, *src_process, found->source_address,
                                     found->target_address, copy_size);
                }

                VAddr prev_reserve = page_start - Memory::CITRA_PAGE_SIZE;
                VAddr next_reserve = page_start + num_pages * Memory::CITRA_PAGE_SIZE;

                auto& prev_vma = src_process->vm_manager.FindVMA(prev_reserve)->second;
                auto& next_vma = src_process->vm_manager.FindVMA(next_reserve)->second;
                ASSERT(prev_vma.meminfo_state == MemoryState::Reserved &&
                       next_vma.meminfo_state == MemoryState::Reserved);

                // Unmap the buffer and guard pages from the source process
                Result result =
                    src_process->vm_manager.UnmapRange(page_start - Memory::CITRA_PAGE_SIZE,
                                                       (num_pages + 2) * Memory::CITRA_PAGE_SIZE);
                ASSERT(result == ResultSuccess);

                mapped_buffer_context.erase(found);

                i += 1;
                break;
            }

            VAddr target_address = 0;

            // TODO(Subv): Perform permission checks.

            // Create a buffer which contains the mapped buffer and two additional guard pages.
            std::shared_ptr<BackingMem> buffer =
                std::make_shared<BufferMem>((num_pages + 2) * Memory::CITRA_PAGE_SIZE);
            memory.ReadBlock(*src_process, source_address,
                             buffer->GetPtr() + Memory::CITRA_PAGE_SIZE + page_offset, size);

            // Map the guard pages and mapped pages at once.
            auto target_address_result = dst_process->vm_manager.MapBackingMemoryToBase(
                Memory::IPC_MAPPING_VADDR, Memory::IPC_MAPPING_SIZE, buffer,
                static_cast<u32>(buffer->GetSize()), Kernel::MemoryState::Shared);

            ASSERT_MSG(target_address_result.Succeeded(), "Failed to map target address");
            target_address = target_address_result.Unwrap();

            // Change the permissions and state of the guard pages.
            const VAddr low_guard_address = target_address;
            const VAddr high_guard_address =
                low_guard_address + static_cast<VAddr>(buffer->GetSize()) - Memory::CITRA_PAGE_SIZE;
            ASSERT(dst_process->vm_manager.ChangeMemoryState(
                       low_guard_address, Memory::CITRA_PAGE_SIZE, Kernel::MemoryState::Shared,
                       Kernel::VMAPermission::ReadWrite, Kernel::MemoryState::Reserved,
                       Kernel::VMAPermission::None) == ResultSuccess);
            ASSERT(dst_process->vm_manager.ChangeMemoryState(
                       high_guard_address, Memory::CITRA_PAGE_SIZE, Kernel::MemoryState::Shared,
                       Kernel::VMAPermission::ReadWrite, Kernel::MemoryState::Reserved,
                       Kernel::VMAPermission::None) == ResultSuccess);

            // Get proper mapped buffer address and store it in the cmd buffer.
            target_address += Memory::CITRA_PAGE_SIZE;
            cmd_buf[i++] = target_address + page_offset;

            mapped_buffer_context.push_back({permissions, size, source_address,
                                             target_address + page_offset, std::move(buffer)});

            break;
        }
        default:
            UNIMPLEMENTED_MSG("Unsupported handle translation: {:#010X}", descriptor);
        }
    }

    if (should_record) {
        std::vector<u32> translated_cmdbuf{cmd_buf.begin(), cmd_buf.begin() + command_size};
        if (reply) {
            kernel.GetIPCRecorder().SetReplyInfo(dst_thread, std::move(untranslated_cmdbuf),
                                                 std::move(translated_cmdbuf));
        } else {
            kernel.GetIPCRecorder().SetRequestInfo(src_thread, std::move(untranslated_cmdbuf),
                                                   std::move(translated_cmdbuf), dst_thread);
        }
    }

    memory.WriteBlock(*dst_process, dst_address, cmd_buf.data(), command_size * sizeof(u32));

    return ResultSuccess;
}

template <class Archive>
void MappedBufferContext::serialize(Archive& ar, const unsigned int) {
    ar & permissions;
    ar & size;
    ar & source_address;
    ar & target_address;
    ar & buffer;
}
SERIALIZE_IMPL(MappedBufferContext)

} // namespace Kernel
