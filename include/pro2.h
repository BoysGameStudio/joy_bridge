#ifndef _PRO2_H_
#define _PRO2_H_

/* Pro2-specific device init and pairing persistence. */

int pro2_device_init(void);
int pro2_pairing_info_save(void);
int pro2_pairing_info_erase(void);

#endif /* _PRO2_H_ */
