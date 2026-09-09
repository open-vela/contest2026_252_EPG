/****************************************************************************
 * boards/arm/bk7258/contest_board/src/bk7258_battery.h
 *
 * Battery gauge for the ConvoAI Kit R1.  Board level rather than chip
 * level: the converter is on the SoC, but the charger, the two status
 * pins and the discharge curve all belong to this board.
 *
 ****************************************************************************/

#ifndef __BOARDS_ARM_BK7258_CONTEST_BOARD_SRC_BK7258_BATTERY_H
#define __BOARDS_ARM_BK7258_CONTEST_BOARD_SRC_BK7258_BATTERY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_battery_register
 *
 * Description:
 *   Configure the charger's two status pins as inputs and register the
 *   gauge at devpath, conventionally "/dev/batt0".
 *
 * Returned Value:
 *   OK, or a negated errno.
 *
 ****************************************************************************/

int bk7258_battery_register(FAR const char *devpath);

#endif /* __BOARDS_ARM_BK7258_CONTEST_BOARD_SRC_BK7258_BATTERY_H */
