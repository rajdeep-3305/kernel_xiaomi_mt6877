/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_CPUFREQ_THERMAL_H
#define __MTK_CPUFREQ_THERMAL_H

struct mt_cpu_dvfs;
struct cpufreq_policy;

#if IS_ENABLED(CONFIG_MTK_MT6877_CPU_THERMAL)
void mt_ppm_cpu_thermal_protect(unsigned int limited_power);
unsigned int mt_ppm_thermal_get_min_power(void);
unsigned int mt_ppm_thermal_get_max_power(void);
unsigned int mt_ppm_thermal_get_cur_power(void);
int mtk_cpu_thermal_prepare(struct cpufreq_policy *policy,
			    const struct mt_cpu_dvfs *dvfs);
void mtk_cpu_thermal_ready(struct cpufreq_policy *policy);
void mtk_cpu_thermal_exit(struct cpufreq_policy *policy);
#else
static inline int mtk_cpu_thermal_prepare(struct cpufreq_policy *policy,
					 const struct mt_cpu_dvfs *dvfs)
{
	return 0;
}
static inline void mtk_cpu_thermal_ready(struct cpufreq_policy *policy) { }
static inline void mtk_cpu_thermal_exit(struct cpufreq_policy *policy) { }
#endif

#endif
