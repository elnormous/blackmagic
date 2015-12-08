/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
** 
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
** 
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#ifndef __BLACKMAGIC_IML__
#define __BLACKMAGIC_IML__

#define BLACKMAGIC_DEV_HAS_SERIAL		1 << 1


enum {
	DL_INTERRUPT_NONE			= 0x0,
	DL_INTERRUPT_HANDLED		= 0x1,
	DL_INTERRUPT_SCHED_WORK		= 0x2,
	DL_INTERRUPT_SCHED_TASKLET	= 0x4,
};

/* Init and Startup */
extern int blackmagic_ioctl_private(void *, void *, unsigned int, unsigned long);
extern void *dl_alloc_driver(void);
extern int dl_start_driver(void *, void *, void *, unsigned int* flags);
extern void *dl_create_and_init_user_client(void *, void *);
extern void dl_release_user_client(void *);
extern void dl_shutdown_driver(void *);
extern unsigned int dl_driver_do_poll(void *, void *, void *);

extern int dl_mmap_buffer(void *ul_ptr, unsigned int type, void** buffer, unsigned long* size);

extern void blackmagic_suspend_driver(void *);
extern void blackmagic_resume_driver(void *);

extern void dl_free_driver(void *);

/* Serial Device Handling */
extern void blackmagic_serial_rx_interrupt(void *);
extern void blackamgic_serial_tx_interrupt(void *, int);

extern void blackmagic_serial_write_byte_priv(void *, unsigned char);
extern void blackmagic_serial_write_byte_size_priv(void *, unsigned int);
extern unsigned char blackmagic_serial_read_byte_priv(void *);
extern unsigned int blackmagic_serial_read_len_priv(void *);
extern void blackmagic_serial_clear_rx_buffer(void *);

/* access to serial port through DeckLink IOCTLs */
extern int blackmagic_serial_open_ioctl(void *);
extern int blackmagic_serial_close_ioctl(void *);
extern int blackmagic_serial_dequeue_data(void *, unsigned char *, int);
extern int blackmagic_serial_enqueue_data(void *, const unsigned char *, int);
extern int blackmagic_serial_port_is_in_use(void *);
extern int blackmagic_serial_port_path(void *, char*, int);

/* Interrupt handling */
extern unsigned int dl_interrupt_handler(void *data);
extern unsigned int dl_tasklet_handler(void *data);
extern unsigned int dl_tasklet_handler_gated(void *data);
extern void dl_bh_work_handler(void *data);

extern bool dl_pci_start(void *pci_dev);
extern void dl_pci_stop(void *pci_dev);
extern bool dl_pci_register_interrupt(void *pci_dev, int source);
extern void dl_pci_unregister_interrupt(void *pci_dev);

void blackmagic_lib_init(void);
void blackmagic_lib_destroy(void);


#endif
