# ##############################################################################
# apps/CMakeLists.txt
#
# Contest board applications.  This directory is reached from the apps build
# through vendor/openvela/apps (a symlink back into the contest tree), so
# application code lives beside the board port it belongs to.
#
# ##############################################################################

# The subdirectory scan below also picks up tests/, which is a symlink to
# ../tests.  nuttx_add_subdirectory() and nuttx_generate_kconfig() only look
# at immediate children, so a directory has to appear here to be built at
# all -- but the self tests belong beside the board port rather than inside
# the shipping application set, so the directory lives one level up and the
# symlink brings it into the scan.  Same arrangement, and same reason, as
# vendor/openvela/apps pointing back into this tree.

nuttx_add_subdirectory()
nuttx_generate_kconfig(MENUDESC "Contest board apps")
