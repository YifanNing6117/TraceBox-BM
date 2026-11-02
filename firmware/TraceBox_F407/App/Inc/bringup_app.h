#ifndef BRINGUP_APP_H
#define BRINGUP_APP_H

#include <stdint.h>

void BringupApp_Init(uint32_t reset_flags);
void BringupApp_MainFunction(uint32_t now_ms);

#endif /* BRINGUP_APP_H */
