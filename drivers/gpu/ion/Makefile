ccflags-y := -Ofast -ffast-math -fgcse-lm -fgcse-sm -fsched-spec-load -fforce-addr -fsingle-precision-constant -mtune=cortex-a9 -marm -march=armv7-a -mfpu=neon -ftree-vectorize -mvectorize-with-neon-double
obj-$(CONFIG_ION) +=	ion.o ion_heap.o ion_system_heap.o ion_carveout_heap.o ion_iommu_heap.o ion_cp_heap.o
obj-$(CONFIG_ION_TEGRA) += tegra/
obj-$(CONFIG_ION_MSM) += msm/
