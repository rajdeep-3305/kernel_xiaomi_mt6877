/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MT6877_DVFSRC_H
#define __MT6877_DVFSRC_H

/* MT6877 legacy SPM/QoS backend, not the newer generic DVFSRC interface. */
int mt6877_dvfsrc_ready(void);
int mt6877_dvfsrc_sync(void);
int mt6877_dvfsrc_wait_vcore(unsigned int opp);
void mt6877_dvfsrc_release_bootstrap(void);

#endif
