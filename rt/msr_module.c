/******************************************************************************
 *
 *  msr_module.c
 *
 *  Kernelmodul f��r 2.6 Kernel zur Me��datenerfassung, Steuerung und Regelung.
 *
 *  Autor: Wilhelm Hagemeister, Florian Pose
 *
 *  (C) Copyright IgH 2002
 *  Ingenieurgemeinschaft IgH
 *  Heinz-B��cker Str. 34
 *  D-45356 Essen
 *  Tel.: +49 201/61 99 31
 *  Fax.: +49 201/61 98 36
 *  E-mail: hm@igh-essen.com
 *
 *  $Id$
 *
 *****************************************************************************/

// Linux
#include <linux/module.h>
#include <linux/ipipe.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>

// RT_lib
#include <msr_main.h>
#include <msr_utils.h>
#include <msr_messages.h>
#include <msr_float.h>
#include <msr_reg.h>
#include <msr_time.h>
#include "msr_param.h"

// EtherCAT
#include "../include/ecrt.h"

#define ASYNC

// Defines/Makros
#define HZREDUCTION (MSR_ABTASTFREQUENZ / HZ)

/*****************************************************************************/
/* Globale Variablen */

// Adeos
static struct ipipe_domain this_domain;
static struct ipipe_sysinfo sys_info;

// EtherCAT
ec_master_t *master = NULL;
ec_domain_t *domain1 = NULL;
ec_domain_t *domain2 = NULL;

// Prozessdaten
void *r_ssi;
void *r_ssi2;
void *r_inc;

uint32_t k_angle;
uint32_t k_pos;
uint32_t k_preio;
uint32_t k_postio;
uint32_t k_finished;

ec_field_init_t domain1_fields[] = {
    {&r_ssi,  "1", "Beckhoff", "EL5001", "InputValue", 0},
    {}
};

ec_field_init_t domain2_fields[] = {
    {&r_ssi2,  "1", "Beckhoff", "EL5001", "InputValue", 0},
    {}
};

/*****************************************************************************/

static void msr_controller_run(void)
{
    cycles_t offset;
    static unsigned int counter = 0;

    offset = get_cycles();

    if (counter) counter--;
    else {
        //EtherCAT_rt_master_debug(master, 2);
        counter = MSR_ABTASTFREQUENZ;
    }

    k_preio = (uint32_t) (get_cycles() - offset) * 1e6 / cpu_khz;

#ifdef ASYNC
    // Empfangen
    ecrt_master_async_receive(master);
    ecrt_domain_process(domain1);
    ecrt_domain_process(domain2);

    // Prozessdaten verarbeiten
    k_pos = EC_READ_U32(r_ssi);

    // Senden
    ecrt_domain_queue(domain1);
    ecrt_domain_queue(domain2);
    ecrt_master_async_send(master);
#else
    // Senden und empfangen
    ecrt_domain_queue(domain1);
    ecrt_domain_queue(domain2);
    ecrt_master_sync_io(master);
    ecrt_domain_process(domain1);
    ecrt_domain_process(domain2);

    // Prozessdaten verarbeiten
    k_pos = EC_READ_U32(r_ssi);
#endif

    k_postio = (uint32_t) (get_cycles() - offset) * 1e6 / cpu_khz;

    //ecrt_master_debug(master, 0);
    k_finished = (uint32_t) (get_cycles() - offset) * 1e6 / cpu_khz;
}

/*****************************************************************************/

int msr_globals_register(void)
{
    msr_reg_kanal("/angle0", "", &k_angle, TUINT);
    msr_reg_kanal("/pos0",   "", &k_pos,   TUINT);

    msr_reg_kanal("/Timing/Pre-IO",   "ns", &k_preio,    TUINT);
    msr_reg_kanal("/Timing/Post-IO",  "ns", &k_postio,   TUINT);
    msr_reg_kanal("/Timing/Finished", "ns", &k_finished, TUINT);

    return 0;
}

/*****************************************************************************/

void msr_run(unsigned irq)
{
    static int counter = 0;

    MSR_ADEOS_INTERRUPT_CODE(msr_controller_run(); msr_write_kanal_list(););

    ipipe_control_irq(irq, 0, IPIPE_ENABLE_MASK); // Interrupt best��tigen
    if (++counter >= HZREDUCTION) {
	ipipe_propagate_irq(irq);  // und weiterreichen
	counter = 0;
    }
}

/*****************************************************************************/

void domain_entry(void)
{
    printk("Domain %s started.\n", ipipe_current_domain->name);

    ipipe_get_sysinfo(&sys_info);
    ipipe_virtualize_irq(ipipe_current_domain,sys_info.archdep.tmirq,
			 &msr_run, NULL, IPIPE_HANDLE_MASK);

    ipipe_tune_timer(1000000000UL / MSR_ABTASTFREQUENZ, 0);
}

/*****************************************************************************/

int __init init_rt_module(void)
{
    struct ipipe_domain_attr attr; //ipipe
    uint32_t version;

    // Als allererstes die RT-lib initialisieren
    if (msr_rtlib_init(1, MSR_ABTASTFREQUENZ, 10, &msr_globals_register) < 0) {
        msr_print_warn("msr_modul: can't initialize rtlib!");
        goto out_return;
    }

    if ((master = ecrt_request_master(0)) == NULL) {
        printk(KERN_ERR "Error requesting master 0!\n");
        goto out_msr_cleanup;
    }

    //ecrt_master_print(master);

    printk(KERN_INFO "Registering domains...\n");

    if (!(domain1 = ecrt_master_create_domain(master))) {
        printk(KERN_ERR "EtherCAT: Could not register domain!\n");
        goto out_release_master;
    }

    if (!(domain2 = ecrt_master_create_domain(master))) {
        printk(KERN_ERR "EtherCAT: Could not register domain!\n");
        goto out_release_master;
    }

    printk(KERN_INFO "Registering domain fields...\n");

    if (ecrt_domain_register_field_list(domain1, domain1_fields)) {
        printk(KERN_ERR "Failed to register domain fields.\n");
        goto out_release_master;
    }

    if (ecrt_domain_register_field_list(domain2, domain2_fields)) {
        printk(KERN_ERR "Failed to register domain fields.\n");
        goto out_release_master;
    }

    printk(KERN_INFO "Activating master...\n");

    //ecrt_master_debug(master, 2);

    if (ecrt_master_activate(master)) {
        printk(KERN_ERR "Could not activate master!\n");
        goto out_release_master;
    }

    //ecrt_master_debug(master, 0);

#if 1
    if (ecrt_master_sdo_read(master, "6", 0x100A, 1, &version)) {
        printk(KERN_ERR "Could not read SSI version!\n");
        goto out_release_master;
    }
    printk(KERN_INFO "Software-version: %u\n", version);
#endif

#ifdef ASYNC
    // Einmal senden und warten...
    ecrt_master_prepare_async_io(master);
#endif

    ipipe_init_attr(&attr);
    attr.name = "IPIPE-MSR-MODULE";
    attr.priority = IPIPE_ROOT_PRIO + 1;
    attr.entry = &domain_entry;
    ipipe_register_domain(&this_domain, &attr);

    return 0;

 out_release_master:
    ecrt_release_master(master);

 out_msr_cleanup:
    msr_rtlib_cleanup();

 out_return:
    return -1;
}

/*****************************************************************************/

void __exit cleanup_rt_module(void)
{
    msr_print_info("msk_modul: unloading...");

    ipipe_tune_timer(1000000000UL / HZ, 0); //alten Timertakt wieder herstellen
    ipipe_unregister_domain(&this_domain);

    if (master)
    {
        printk(KERN_INFO "=== Stopping EtherCAT environment... ===\n");

        printk(KERN_INFO "Deactivating master...\n");
        ecrt_master_deactivate(master);
        ecrt_release_master(master);

        printk(KERN_INFO "=== EtherCAT environment stopped. ===\n");
    }

    msr_rtlib_cleanup();
}

/*****************************************************************************/

#define EC_LIT(X) #X
#define EC_STR(X) EC_LIT(X)
#define COMPILE_INFO "Revision " EC_STR(SVNREV) \
                     ", compiled by " EC_STR(USER) \
                     " at " __DATE__ " " __TIME__

MODULE_LICENSE("GPL");
MODULE_AUTHOR ("Florian Pose <fp@igh-essen.com>");
MODULE_DESCRIPTION ("EtherCAT real-time test environment");
MODULE_VERSION(COMPILE_INFO);

module_init(init_rt_module);
module_exit(cleanup_rt_module);

/*****************************************************************************/

/* Emacs-Konfiguration
;;; Local Variables: ***
;;; c-basic-offset:4 ***
;;; End: ***
*/
