# ims_bridge: the media plane of a VoLTE video call. The modem hands the call's video RTP to the AP over SIPC
# (sblocks IMSBR_DATA/IMSBR_CTRL on the modem's channel) and sends what the AP writes back into the IMS PDN; a program
# on the AP that bridges the call then sees the video as plain UDP on an interface of its own. Unisoc's net/ims_bridge
# from the realme AndroidU 5.15 tree (GPL-2.0), ported to 6.18 and 7.2 (docs/FINDINGS.md 192). It talks to the modem
# over SIPC, so it needs the modem stack's headers and its exported symbols. imsbr_transit (the orca boards') and the
# vendor's debugfs self-test (CONFIG_SPRD_IMS_BRIDGE_TEST) are left out.
obj-m += ims_bridge.o
ims_bridge-objs := imsbr_main.o imsbr_hooks.o imsbr_sipc.o imsbr_packet.o imsbr_core.o imsbr_netlink.o
ccflags-y += -I$(src)/../sprd_modem/kinclude -I$(src)/include
