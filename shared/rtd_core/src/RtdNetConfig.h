#ifndef RTD_NET_CONFIG_H
#define RTD_NET_CONFIG_H

/* Default addresses. Devices also learn addresses at run time (DESIGN.md 3.2). */
#define RTD_MAC_HOST    {0x88, 0x57, 0x21, 0xB3, 0x05, 0xAC}
#define RTD_MAC_DISPLAY {0xD0, 0xCF, 0x13, 0x01, 0xD1, 0xA4}
#define RTD_MAC_STICK1  {0xBC, 0xFF, 0x4D, 0xF9, 0xF3, 0x91}   /* white  */
#define RTD_MAC_STICK2  {0xBC, 0xFF, 0x4D, 0xF9, 0xAE, 0x29}   /* blue   */
#define RTD_MAC_STICK3  {0xBC, 0xFF, 0x4D, 0xF9, 0xAC, 0x42}   /* red    */
#define RTD_MAC_STICK4  {0xBC, 0xFF, 0x4D, 0xF9, 0xBE, 0x62}   /* yellow */

/* Maintenance access point of the host (OTA).
 * Set your own password in RtdSecrets.h next to this file (not in git):
 *   #define RTD_AP_PASS "your-password"   (8 characters or more)
 * All devices must be built with the same password. */
#if defined(__has_include)
#if __has_include("RtdSecrets.h")
#include "RtdSecrets.h"
#endif
#endif
#define RTD_AP_SSID     "RTD-Host"
#ifndef RTD_AP_PASS
#define RTD_AP_PASS     "change-me-1234"   /* placeholder */
#endif
#define RTD_HOST_IP     "192.168.4.1"

#endif /* RTD_NET_CONFIG_H */
