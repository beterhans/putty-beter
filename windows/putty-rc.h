/*
 * putty-rc.h - constants shared between putty-common.rc2 and the C code.
 */

#ifndef PUTTY_WIN_RES_H
#define PUTTY_WIN_RES_H

#define IDI_MAINICON     200
#define IDI_CFGICON      201

#define IDD_MAINBOX      102
#define IDD_LOGBOX       110
#define IDD_ABOUTBOX     111
#define IDD_RECONF       112
#define IDD_LICENCEBOX   113
#define IDD_HOSTKEY      114
#define IDD_HK_MOREINFO  116
#define IDD_CA_CONFIG    117
#define IDD_CONFIG_LOCATION 118
#define IDD_ZMODEM       119

/* PuTTY-beter: the configuration-folder dialog. */
#define IDC_CL_MSG      1101
#define IDC_CL_PATH     1102
#define IDC_CL_BROWSE   1103
#define IDC_CL_HINT     1104

/* PuTTY-beter: the ZMODEM transfer progress window (tech_detail.md section 12). */
#define IDC_ZM_STATUS   1111
#define IDC_ZM_PROGRESS 1112
#define IDC_ZM_CANCEL   1113

#define IDN_LIST        1001
#define IDN_COPY        1002

#define IDA_ICON        1001
#define IDA_TEXT        1002
#define IDA_LICENCE     1003
#define IDA_WEB         1004

#define IDC_TAB         1001
#define IDC_TABSTATIC1  1002
#define IDC_TABSTATIC2  1003
#define IDC_TABLIST     1004
#define IDC_HELPBTN     1005
#define IDC_ABOUT       1006

#define IDC_HK_ICON       98
#define IDC_HK_TITLE      99
#define IDC_HK_TEXT      100
#define IDC_HK_ACCEPT   1001
#define IDC_HK_ONCE     1000
#define IDC_HK_HOST     1002
#define IDC_HK_FINGERPRINT 1003
#define IDC_HK_MOREINFO 1004

#define IDC_HKI_SHA256  1000
#define IDC_HKI_MD5     1001
#define IDC_HKI_PUBKEY  1002

#define ID_CUSTOM_CHMFILE 2000
#define TYPE_CUSTOM_CHMFILE 2000

#endif
