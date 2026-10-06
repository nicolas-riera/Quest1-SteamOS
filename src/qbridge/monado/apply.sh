#!/bin/bash
# Graft the qbridge driver, builder and compositor target into a Monado tree.
# usage: apply.sh <monado_src_dir>   (idempotent)
set -e
M=${1:?monado dir}; Q=$(cd "$(dirname "$0")" && pwd)
X=$M/src/xrt
mkdir -p $X/drivers/qbridge
cp $Q/drivers/qbridge/* $Q/../linux/qb_link.c $Q/../linux/qb_link.h $Q/../common/qbridge_proto.h $X/drivers/qbridge/
bash $Q/apply_ahb.sh $M
cp $Q/compositor/main/comp_window_qbridge.c $X/compositor/main/
cp $Q/targets/common/target_builder_qbridge.c $X/targets/common/

grep -q XRT_BUILD_DRIVER_QBRIDGE $M/CMakeLists.txt || \
  sed -i 's|^option(XRT_BUILD_DRIVER_SIMULATED "Enable simulated driver" ON)|&\noption(XRT_BUILD_DRIVER_QBRIDGE "Enable qbridge (Quest via Meta runtime) driver" ON)|' $M/CMakeLists.txt

grep -q drv_qbridge $X/drivers/CMakeLists.txt || cat >> $X/drivers/CMakeLists.txt <<'C'

if(XRT_BUILD_DRIVER_QBRIDGE)
	add_library(drv_qbridge STATIC qbridge/qbridge_device.c qbridge/qb_link.c qbridge/qbridge_interface.h)
	target_include_directories(drv_qbridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} qbridge /opt/hybris/include)
	target_link_libraries(drv_qbridge PRIVATE xrt-interfaces aux_util aux_math aux_os)
	target_link_libraries(drv_qbridge PUBLIC -L/opt/hybris/lib -lhybris-common)
	list(APPEND ENABLED_HEADSET_DRIVERS qbridge)
endif()
C

grep -q target_builder_qbridge $X/targets/common/CMakeLists.txt || cat >> $X/targets/common/CMakeLists.txt <<'C'

if(XRT_BUILD_DRIVER_QBRIDGE)
	target_sources(target_lists PRIVATE target_builder_qbridge.c)
	target_link_libraries(target_lists PRIVATE drv_qbridge)
endif()
C

if ! grep -q t_builder_qbridge_create $X/targets/common/target_lists.c; then
  sed -i 's|^xrt_builder_create_func_t target_builder_list\[\] = {|&\n#ifdef XRT_BUILD_DRIVER_QBRIDGE\n    t_builder_qbridge_create,\n#endif|' $X/targets/common/target_lists.c
fi
grep -q t_builder_qbridge_create $X/targets/common/target_builder_interface.h || \
  sed -i 's|^#ifdef T_BUILDER_SIMULATED|#ifdef XRT_BUILD_DRIVER_QBRIDGE\nstruct xrt_builder *\nt_builder_qbridge_create(void);\n#endif\n\n&|' $X/targets/common/target_builder_interface.h

grep -q comp_window_qbridge.c $X/compositor/main/CMakeLists.txt || \
  sed -i 's|^\tcomp_window_debug_image.c|&\n\tcomp_window_qbridge.c|' $X/compositor/main/CMakeLists.txt
grep -q drv_qbridge $X/compositor/main/CMakeLists.txt || \
  printf '\nif(XRT_BUILD_DRIVER_QBRIDGE)\n\ttarget_link_libraries(comp_main PRIVATE drv_qbridge)\nendif()\n' >> $X/compositor/main/CMakeLists.txt
grep -q comp_target_factory_qbridge $X/compositor/main/comp_window.h || \
  sed -i 's|^extern const struct comp_target_factory comp_target_factory_debug_image;|&\nextern const struct comp_target_factory comp_target_factory_qbridge;|' $X/compositor/main/comp_window.h
grep -q "&comp_target_factory_qbridge" $X/compositor/main/comp_compositor.c || \
  sed -i 's|^    &comp_target_factory_debug_image,|    \&comp_target_factory_qbridge,\n&|' $X/compositor/main/comp_compositor.c

# xrt_config_drivers.h defines XRT_BUILD_DRIVER_<X> for each entry of AVAILABLE_DRIVERS.
grep -q '"QBRIDGE"' $M/CMakeLists.txt || sed -i '0,/^\t"SIMULATED"$/s//\t"SIMULATED"\n\t"QBRIDGE"/' $M/CMakeLists.txt
echo "qbridge grafted into $M"
