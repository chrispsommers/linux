#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x27126e5a, "module_layout" },
	{ 0x6b10bee1, "_copy_to_user" },
	{ 0x409bcb62, "mutex_unlock" },
	{ 0xb5136dc7, "mutex_lock_interruptible" },
	{ 0x13c49cc2, "_copy_from_user" },
	{ 0x56019ef0, "cdev_del" },
	{ 0x2bee6d6d, "device_destroy" },
	{ 0x37a0cba, "kfree" },
	{ 0x6091b333, "unregister_chrdev_region" },
	{ 0x98a295b0, "class_destroy" },
	{ 0x689b71c8, "sysfs_remove_file_ns" },
	{ 0xc68aeac, "kobject_put" },
	{ 0xcd2461e1, "sysfs_create_file_ns" },
	{ 0x55a25eb9, "kobject_create_and_add" },
	{ 0x825a2227, "kernel_kobj" },
	{ 0xf57bae51, "kmem_cache_alloc_trace" },
	{ 0x2e8895d0, "kmalloc_caches" },
	{ 0xfaad79e, "device_create" },
	{ 0x20dde6b3, "__class_create" },
	{ 0xa00aaa80, "cdev_add" },
	{ 0x159131d9, "cdev_init" },
	{ 0xe3ec2f2b, "alloc_chrdev_region" },
	{ 0x3c3ff9fd, "sprintf" },
	{ 0xc5850110, "printk" },
	{ 0xbcab6ee6, "sscanf" },
	{ 0xbdfb6dbb, "__fentry__" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "664B469E248BED6465F5824");
