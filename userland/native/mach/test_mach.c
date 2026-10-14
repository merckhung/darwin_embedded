/* Exercise Apple's actual Mach wrappers and generated IPC clients in XNU. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_info.h>
#include <stdint.h>

extern int mach_init(void);
extern uint64_t __thread_selfid(void);

int
native_mach_probe(void)
{
    /* dyld has established native TSD. Apple's mach_init fills actual cached
     * task/reply ports and page sizes directly from traps and the commpage.
     * These operations do not use a malloc or cancellation callback. */
    if (mach_init() != 0 || mach_task_self() == MACH_PORT_NULL ||
        vm_page_size == 0 || (vm_page_size & (vm_page_size - 1)) != 0)
        return 1;

    mach_vm_address_t address = 0;
    mach_vm_size_t size = 2 * vm_page_size;
    if (mach_vm_allocate(mach_task_self(), &address, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return 2;
    volatile unsigned char *bytes = (volatile unsigned char *)(uintptr_t)address;
    for (unsigned i = 0; i != 256; ++i)
        bytes[i] = (unsigned char)(i ^ 0xa7);
    unsigned char copy[256];
    mach_vm_size_t copied = 0;
    /* read_overwrite has no direct trap shortcut: this reaches the genuine
     * MIG request/reply implementation and XNU's Mach IPC dispatcher. */
    if (mach_vm_read_overwrite(mach_task_self(), address, sizeof(copy),
                              (mach_vm_address_t)(uintptr_t)copy, &copied) != KERN_SUCCESS ||
        copied != sizeof(copy))
        return 3;
    for (unsigned i = 0; i != sizeof(copy); ++i)
        if (copy[i] != (unsigned char)(i ^ 0xa7))
            return 4;
    if (mach_vm_protect(mach_task_self(), address, size, FALSE, VM_PROT_READ) != KERN_SUCCESS ||
        mach_vm_deallocate(mach_task_self(), address, size) != KERN_SUCCESS)
        return 5;

    mach_port_t port = MACH_PORT_NULL;
    mach_port_type_t type = 0;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS ||
        mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS ||
        mach_port_type(mach_task_self(), port, &type) != KERN_SUCCESS ||
        (type & MACH_PORT_TYPE_SEND_RECEIVE) != MACH_PORT_TYPE_SEND_RECEIVE)
        return 6;
    if (mach_port_deallocate(mach_task_self(), port) != KERN_SUCCESS ||
        mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1) != KERN_SUCCESS)
        return 7;

    mach_port_t thread = mach_thread_self();
    thread_identifier_info_data_t identity;
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread == MACH_PORT_NULL ||
        thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&identity, &count) != KERN_SUCCESS ||
        count != THREAD_IDENTIFIER_INFO_COUNT || identity.thread_id == 0 ||
        identity.thread_id != __thread_selfid())
        return 8;
    if (mach_port_deallocate(mach_task_self(), thread) != KERN_SUCCESS)
        return 9;
    return 0;
}
