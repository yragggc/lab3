/*
 * linux/arch/arm/mach-omap2/ti-psp-version.c
 *
 * Create a proc entry for showing PSP version.
 *
 * Copyright (C) 2008 Texas Instruments Incorporated.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>

#include "psp-version.h"

static int init_psp_module(void);
static void exit_psp_module(void);


static struct proc_dir_entry	*vers_file;

static int show_version(char *page, char **start,
			     off_t off, int count,
			     int *eof, void *data)
{
	int len;

	len = sprintf(page, TI_PSP_DEVICE \
			" Linux PSP version " TI_PSP_VERSION \
			" (" TI_PSP_PLATFORM ")\n");

	return len;
}

static int __init init_psp_module(void)
{
	int result = 0;

	/*
	 * Create the file "/proc/ti-psp-version"
	 */
	vers_file = create_proc_read_entry(
			TI_PSP_ENTRY,
			0444,
			NULL,
			show_version,
			NULL);

	if (vers_file != NULL) {
		vers_file->owner = THIS_MODULE;

		printk (KERN_INFO TI_PSP_DEVICE \
					" Linux PSP version " TI_PSP_VERSION \
					" (" TI_PSP_PLATFORM ")\n");
	}
	else {
		result = -ENOMEM;
	}

	return result;	
}

static void __exit exit_psp_module(void)
{
	remove_proc_entry(TI_PSP_ENTRY, NULL);
}

module_init(init_psp_module);
module_exit(exit_psp_module);

MODULE_LICENSE("GPLv2");
MODULE_AUTHOR("Texas Instruments Incorporated");
MODULE_DESCRIPTION("Contains version of the Linux PSP release.");
