#ifndef _SNMP_H_
#define _SNMP_H_

#include <stdint.h>

#define SNMP_TEXT_MAX	48

struct snmp_config {
	uint8_t on;
	char community[SNMP_TEXT_MAX + 1];
	char location[SNMP_TEXT_MAX + 1];
	char contact[SNMP_TEXT_MAX + 1];
};

extern __xdata struct snmp_config snmp_cfg;

void snmp_init(void) __banked;
void snmp_callback(uint16_t lport) __banked;
void snmp_cmd(void) __banked;
void send_snmp(void) __banked;

#endif
