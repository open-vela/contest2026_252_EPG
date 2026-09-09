/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_phy_osi.c
 *
 * OS/SoC abstraction tables for the closed Beken RF PHY archives
 * (libbk_phy.a / libcom_phy.a).
 *
 * Like the bluetooth controller, the PHY archive carries no SoC glue: it
 * reaches everything -- analog register access, power and clock gates,
 * calibration storage, logging, allocation -- through two function-pointer
 * tables handed to phy_adapter_init(), plus a third pair handed to
 * rf_adapter_init() that the RF arbiter (rf_module_vote_ctrl) uses.  Until
 * those tables are installed every pointer in them is NULL, which is the
 * bus fault at rwip_init -> xvr_init -> bt_osi_ble_vote_rf_ctrl ->
 * rf_module_vote_ctrl.
 *
 * The four structs below are byte-for-byte replicas of bk_idk
 * components/bk_phy/include/bk_phy_adapter.h and bk_rf_adapter.h with the
 * BK7258 configuration resolved (CONFIG_SOC_BK7236XX=y, CONFIG_SOC_BK7258=y,
 * so neither the BK7256XX nor the BK7239XX/BK7286XX variant of the
 * conditional blocks applies).  The SDK headers are deliberately not
 * included, because this port does not carry the Beken header tree, and the
 * library reads the tables purely by memory layout -- field order and types
 * must not be touched.  The replica was checked against the shipped code
 * rather than against the header alone: rf_module_vote_ctrl's error path
 * loads g_phy_funcs_t offset 0x104 to call the logger, and offset 0x104 is
 * exactly where _log lands in the struct below.
 *
 * Where the vendor calls into an SDK subsystem this port does not have, the
 * entry is a documented stub rather than a guess.  Three port-wide
 * decisions drive most of them, and each affected entry says so again at
 * the point of use:
 *
 *   1. Factory calibration data is unreachable.  The NuttX image is flashed
 *      at 0x11000 while the RF calibration partition sits at 0x7fe000, and
 *      this port has no flash read driver -- the XIP window is CRC-encoded
 *      and cannot be read as a plain data region.  So every "load
 *      calibration from flash/OTP" callback reports "no data", and the
 *      library falls back to the default power tables in bk7258_vnd_cal.c.
 *      Transmit power is therefore nominal rather than per-unit trimmed:
 *      wrong absolute level, correct enough to emit packets, which is the
 *      right trade for a first bring-up.
 *
 *   2. The SARADC is real, driven by bk7258_saradc.c: the transmit-power
 *      detector, the on-die temperature sensor and the supply are all
 *      measured rather than assumed, because the calibration arithmetic
 *      is built on them.  What is still off is the *periodic* re-trim --
 *      _bk_feature_temp_detect_enable reports the feature disabled, since
 *      this port runs no temperature daemon.  Calibration's own one-shot
 *      temperature read does not go through that flag and does happen.
 *
 *   3. Sleep is never entered on this port, so the low-power vote entries
 *      are no-ops.  Note that RF power arbitration is *not* in that
 *      category -- rf_module_vote_ctrl genuinely powers the PHY domain up
 *      and down around radio use, and those entries do real register work.
 *
 * Wi-Fi is not in this image; the Wi-Fi entries are NULL or report "not
 * present" so the library never believes the radio is busy with Wi-Fi.
 *
 * Nothing here runs unless bk7258_phy_adapter_init() and then
 * bk7258_rf_adapter_init() are called, in that order -- the RF arbiter
 * reaches for the PHY table's logger on its error path.  Neither is wired
 * into board bring-up.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/wqueue.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"
#include "bk7258_wdt.h"

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

/* The polled SARADC driver in bk7258_saradc.c.  Declared here rather than
 * in a header because that is how this board reaches its other chip-layer
 * entry points (see apps/face/face_main.c), and because the converter has
 * exactly one consumer: the acquisition entries further down.
 */

uint32_t bk7258_saradc_div(uint32_t adc_clk);
int bk7258_saradc_pwrup(void);
int bk7258_saradc_start(uint8_t channel, uint8_t mode, uint32_t div,
                        uint8_t saturate, uint8_t steady, uint8_t rate,
                        uint8_t filter);
int bk7258_saradc_stop(void);
int bk7258_saradc_read(uint16_t *buf, uint32_t size, uint32_t timeout_ms);
void bk7258_saradc_tempsensor(bool enable);
void bk7258_saradc_set_flag(uint8_t flag);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Handshake the library validates before it uses the PHY table at all
 * (bk_idk components/bk_phy/src/bk_phy_adapter.c PHY_OSI_VERSION).
 */

#define PHY_OSI_VERSION             0x00060006

/* System block registers.  Word indices are the SDK's, kept visible so the
 * addresses can be checked against sys_ll.h: analog shadow registers start
 * at word 0x40 and their busy flags live in word 0x3a, one bit per shadow.
 */

#define PHY_SYS_POWER               (BK7258_SYS_BASE + (0x10 << 2))
#define PHY_SYS_CLK_EN              (BK7258_SYS_BASE + (0x0c << 2))
#define PHY_SYS_ANA_BUSY            (BK7258_SYS_BASE + (0x3a << 2))
#define PHY_SYS_ANA_REG(n)          (BK7258_SYS_BASE + ((0x40 + (n)) << 2))

#define PHY_CKEN_PHY                (1u << 27)

/* Power domain bits in PHY_SYS_POWER are indexed by power_module_name_t and
 * are inverted: 0 powers the domain up.  The PHY domain is index 10.
 */

#define PHY_PWR_MODULE_WIFI_PHY     10
#define PHY_PWR_MODULE_LAST         15

/* Sub-domain identifiers are parent * PM_MODULE_SUB_POWER_DOMAIN_MAX + n
 * (bk7258 hal/sys_types.h, PM_MODULE_SUB_POWER_DOMAIN_MAX == 20), so a
 * sub-domain reduces to its parent by integer division.
 */

#define PHY_PWR_SUB_DOMAIN_STRIDE   20

#define PHY_PWR_STATE_ON            0
#define PHY_PWR_STATE_OFF           1
#define PHY_PWR_STATE_NONE          2

/* AON PMU words the PHY reads for die-specific trim (bk7258
 * soc/aon_pmu_ll.h + aon_pmu_struct.h).
 */

#define PHY_AON_PMU_R7C             (BK7258_AON_PMU_BASE + (0x7c << 2))
#define PHY_AON_PMU_R7D             (BK7258_AON_PMU_BASE + (0x7d << 2))
#define PHY_AON_PMU_R7E             (BK7258_AON_PMU_BASE + (0x7e << 2))

#define PHY_AON_R7D_ADC_CAL_POS     9
#define PHY_AON_R7D_ADC_CAL_MASK    0x3f
#define PHY_AON_R7E_CBCAL_POS       0
#define PHY_AON_R7E_CBCAL_MASK      0x1f

/* Analog shadow register fields, transcribed from bk7258 hal/sys_ll.h.
 * Encoded as index/position/mask triples so the accessors below stay one
 * line each and the transcription can be diffed against the SDK.
 */

#define PHY_ANA_SPIDETEN            0, 4, 0x1
#define PHY_ANA_SPITRIG             0, 19, 0x1
#define PHY_ANA_XTALH_CTUNE         2, 0, 0xff
#define PHY_ANA_ADC_DIV             5, 10, 0x3
#define PHY_ANA_BCAL_START          5, 22, 0x1
#define PHY_ANA_BCAL_EN             5, 23, 0x1
#define PHY_ANA_VBIAS               5, 27, 0x1f
#define PHY_ANA_IOLDO_LP            8, 0, 0x1
#define PHY_ANA_VIOLDOSEL           8, 12, 0x7
#define PHY_ANA_IOCURLIM            8, 15, 0x1
#define PHY_ANA_BGCAL               8, 22, 0x3f
#define PHY_ANA_SPI_LATCH1V         9, 9, 0x1
#define PHY_ANA_VCOREHSEL           9, 16, 0xf
#define PHY_ANA_IOBYAPSSEN          10, 19, 0x1
#define PHY_ANA_APFMS               11, 5, 0x1f
#define PHY_ANA_ALDOSEL             11, 31, 0x1
#define PHY_ANA_DPFMS               12, 8, 0x1f
#define PHY_ANA_DLDOSEL             12, 31, 0x1

/* Vendor default for a pad current limit that is otherwise left alone
 * (bk_phy_adapter.c sys_ll_set_ana_reg8_violdosel_wrapper).
 */

#define PHY_VIOLDOSEL_LIMITED       2
#define PHY_VIOLDOSEL_DEFAULT       4

/* Fallbacks for a sensor read that fails.  Both are raw SARADC codes, not
 * engineering units -- that is what the vendor's own accessors return.
 * The supply code is the one the vendor's driver pins to its 1 V
 * calibration point, so a consumer that ignores the error code sees a
 * stable nominal supply and never applies a droop correction; the
 * temperature code is the midpoint of the vendor's own validity window.
 */

#define PHY_TEMP_RAW_NOMINAL        687
#define PHY_VOLT_RAW_NOMINAL        0x9c7

/* Sensor acquisition, transcribed from the vendor's detectors
 * (bk_idk components/temp_detect/temp_detect.c and volt_detect.c with
 * CONFIG_SOC_BK7236XX set and CONFIG_SDMADC_TEMP unset).  Both run the
 * converter continuously at 203125 Hz with the longest settle, read
 * PHY_ADC_TEMP_BUFFER_SIZE samples, discard the first PHY_ADC_SENSE_SKIP
 * and average what is left; the temperature result is then divided by
 * four.  Channel 7 is the on-die temperature sensor and channel 0 the
 * supply (bk7258 hal/adc_ll.h channel map).
 */

#define PHY_ADC_TEMP_CHANNEL        7
#define PHY_ADC_VOLT_CHANNEL        0
#define PHY_ADC_SENSE_CLK           203125
#define PHY_ADC_SENSE_STEADY        7
#define PHY_ADC_SENSE_SKIP          5
#define PHY_ADC_SENSE_RETRY         3
#define PHY_ADC_SENSE_TIMEOUT_MS    100
#define PHY_ADC_TEMP_SHIFT          2

/* A sample of exactly 2048 is the converter's mid-code and the vendor
 * treats it, like zero, as "no reading" rather than as data.
 */

#define PHY_ADC_SAMPLE_INVALID      2048

/* adc_mode_t and adc_saturate_mode_t (bk_idk driver/hal/hal_adc_types.h).
 * The saturation modes count from "off", so the vendor's mode 3 is 4.
 */

#define PHY_ADC_MODE_CONTINUOUS     3
#define PHY_ADC_SAT_MODE_2          3
#define PHY_ADC_SAT_MODE_3          4

/* SARADC calibration anchors for BK7236XX-class parts, the two codes the
 * vendor's driver pins to 1 V and 2 V (bk_idk driver/saradc/adc_driver.c
 * saradc_val).  Only _saradc_calculate uses them.
 */

#define PHY_SARADC_CODE_1V          0x9c7
#define PHY_SARADC_CODE_2V          0x1358

/* Chip identity, copied from the SDK's bk7258 sys_types.h. */

#define PHY_CHIP_ID_MASK            0xffff0000ul
#define PHY_CHIP_ID_MPW_V2_3        0x22710010ul
#define PHY_CHIP_ID_MPW_V4          0x22c20010ul
#define PHY_CHIP_ID_MP_A            0x23640810ul

/* Temperature detector scaling (bk_idk components/temp_detect/temp_detect.h
 * with CONFIG_SOC_BK7236XX and CONFIG_SDMADC_TEMP off).  Inert here, but the
 * library reads them out of the variable table unconditionally.
 */

#define PHY_ADC_TEMP_10DEG_PER_DBPWR  1
#define PHY_ADC_TEMP_BUFFER_SIZE      (5 + 5)
#define PHY_ADC_TEMP_LSB_PER_10DEG    46
#define PHY_ADC_TEMP_VAL_MIN          10
#define PHY_ADC_TEMP_VAL_MAX          1365
#define PHY_ADC_XTAL_DIST_INITIAL     70
#define PHY_ADC_TEMP_DIST_INITIAL     0

/* Wi-Fi RF config item tags (bk_wifi_types.h) and band indices
 * (modules/wifi_types.h).  Read by paths this image does not run.
 */

#define PHY_RF_CFG_TSSI_ITEM        0x77777777ul
#define PHY_RF_CFG_MODE_ITEM        0x99999999ul
#define PHY_RF_CFG_TSSI_B_ITEM      0xbbbbbbbbul

/* pm_cpu_freq_e / pm_dev_id_e indices (bk_idk include/modules/pm.h). */

#define PHY_PM_CPU_FRQ_60M          1
#define PHY_PM_CPU_FRQ_80M          2
#define PHY_PM_CPU_FRQ_120M         3
#define PHY_PM_CPU_FRQ_320M         5
#define PHY_PM_CPU_FRQ_DEFAULT      7
#define PHY_PM_DEV_ID_PHY           27

/* hardware_chip_version_e (bk_idk include/modules/chip_support.h). */

#define PHY_CHIP_VERSION_A          0
#define PHY_CHIP_VERSION_B          1
#define PHY_CHIP_VERSION_C          2
#define PHY_CHIP_VERSION_DEFAULT    3

/* otp_id_t indices for the calibration items (bk_idk secure_calibration
 * .../driver/otp/hal_otp_types.h).  OTP access below always fails, so these
 * are carried for completeness only.
 */

#define PHY_OTP_MAC_ADDRESS         28
#define PHY_OTP_VDDDIG_BANDGAP      29
#define PHY_OTP_DIA                 30
#define PHY_OTP_SDMADC_CALIBRATION  32
#define PHY_OTP_GADC_TEMPERATURE    35

/* PARAM_XTALH_CTUNE_MASK / PARAM_AUD_DAC_GAIN_MASK for BK7236XX
 * (bk_idk driver/include/bk_private/bk_sys_ctrl.h).
 */

#define PHY_PARAM_XTALH_CTUNE_MASK  0xff
#define PHY_PARAM_AUD_DAC_GAIN_MASK 0x1f

/* Beken return convention: 0 is success, negative is failure. */

#define PHY_OK                      0
#define PHY_FAIL                    (-1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Scalar spellings the replicated tables need; the SDK gets them from
 * common/bk_include.h, which this port does not carry.
 */

typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef int32_t  INT32;
typedef int32_t  int32;
typedef uint64_t uint64;
typedef int      bk_err_t;
typedef void     VOID;

/* Replica of phy_os_funcs_t (bk_phy_adapter.h, lines 14..190) with the
 * BK7258 configuration resolved.  Do not reorder.
 */

typedef struct
{
  uint32_t _version;

  void * (*_mpb_reg_api)(void);
  void * (*_crm_reg_api)(void);
  void * (*_riu_reg_api)(void);
  void * (*_mix_funcs)(void);
  uint32_t (*_bk_misc_get_reset_reason)(void);
  void (*_rs_init)(UINT32 channel, INT32 band, UINT32 mode);
  void (*_rs_bypass_mac_init)(UINT32 channel, INT32 band, UINT32 mode);
  void (*_rs_deinit)(void);
  void (*_evm_init)(UINT32 channel, INT32 band, UINT32 bandwidth);
  void (*_evm_bypass_mac_init)(UINT32 frequency, INT32 band,
                               UINT32 bandwidth);

  void (*_nv_phy_reg_set_hook)(void *hook);
  void (*_evm_clear_ke_evt_mac_bit)(void);
  void (*_evm_set_ke_evt_mac_bit)(void);
  void (*_tx_evm_set_chan_ctxt_pop)(void *chan_info);

  UINT32 (*_save_info_item)(UINT32 item, UINT8 *ptr0, UINT8 *ptr1,
                            UINT8 *ptr2);
  UINT32 (*_get_info_item)(UINT32 item, UINT8 *ptr0, UINT8 *ptr1,
                           UINT8 *ptr2);

  void (*_delay)(int32 num);
  void (*_delay_us)(UINT32 us);
  UINT32 (*_ddev_control)(UINT32 handle, UINT32 cmd, VOID *param);
  int (*_bk_wdt_stop)(void);

  bool (*_temp_detect_is_init)(void);
  int (*_temp_detect_deinit)(void);
  int (*_temp_detect_init)(uint32_t init_temperature);
  int (*_temp_detect_get_temperature)(uint32_t *temperature);
  int (*_bk_feature_temp_detect_enable)(void);

  bool (*_ate_is_enabled)(void);
  int (*_volt_single_get_current_voltage)(UINT32 *volt_value);
  float (*_saradc_calculate)(UINT16 adc_val);

  int (*_bk_pm_clock_ctrl_saradc_pwrup)(void);
  int (*_bk_pm_clock_ctrl_phy_pwrup)(void);
  int (*_bk_pm_module_vote_power_ctrl_phy)(int32_t value);
  int (*_bk_pm_module_vote_cpu_freq)(uint32_t module, uint32_t cpu_freq);

  uint32_t (*_aon_pmu_hal_get_chipid)(void);
  uint32_t (*_aon_pmu_drv_bias_cal_get)(void);
  uint32_t (*_aon_pmu_drv_get_adc_cal)(void);

  void (*_sys_ll_set_ana_reg5_adc_div)(uint32_t value);
  uint32_t (*_sys_ll_get_ana_reg5_adc_div)(void);
  void (*_sys_ll_set_ana_reg8_ioldo_lp)(uint32_t value);
  void (*_sys_ll_set_ana_reg9_vcorehsel)(uint32_t value);
  void (*_sys_ll_set_ana_reg9_spi_latch1v)(uint32_t value);
  void (*_sys_ll_set_ana_reg10_iobyapssen)(uint32_t value);
  void (*_sys_ll_set_ana_reg11_aldosel)(uint32_t value);
  void (*_sys_ll_set_ana_reg12_dldosel)(uint32_t value);

  uint32_t (*_sys_drv_cali_dpll)(uint32_t param);
  void (*_sys_drv_set_ana_ioldo_lp)(uint32_t value);
  void (*_sys_drv_set_ana_cb_cal_manu_val)(uint32_t value);
  void (*_sys_drv_set_ana_cb_cal_trig)(uint32_t value);
  void (*_sys_drv_set_ana_cb_cal_manu)(uint32_t value);
  uint32_t (*_sys_drv_analog_set_xtalh_ctune)(uint32_t param);

  void (*_phy_sys_drv_modem_bus_clk_ctrl_on)(void);
  void (*_phy_sys_drv_modem_clk_ctrl_on)(void);

  int (*_bk_adc_read_raw)(uint16_t *buf, uint32_t size, uint32_t timeout);

  int (*_bk_cal_saradc_start)(int32_t adc_channel, int32_t adc_clk,
                              int32_t steady_time);
  int (*_bk_cal_saradc_stop)(int32_t adc_channel);

  int (*_bk_saradc_start)(uint8_t adc_channel, uint32_t sample_rate,
                          uint32_t div, uint32_t saradc_clk,
                          uint32_t saradc_mode, uint32_t saradc_xtal,
                          uint32_t saturate_mode, uint32_t steady_time);
  int (*_bk_saradc_stop)(uint8_t adc_channel);

  int (*_bk_flash_read_bytes)(uint32_t address, uint8_t *user_buf,
                              uint32_t size);
  int (*_bk_flash_erase_sector)(uint32_t address);
  int (*_bk_flash_write_bytes)(uint32_t address, const uint8_t *user_buf,
                               uint32_t size);

  bk_err_t (*_bk_flash_set_protect_type_protect_none)(void);
  bk_err_t (*_bk_flash_set_protect_type_unprotect_last_block)(void);
  uint32_t (*_bk_flash_partition_get_rf_firmware_info)(void);

  int (*_bk_otp_apb_read)(uint32_t item, uint8_t *buf, uint32_t size);
  int (*_bk_otp_apb_update)(uint32_t item, uint8_t *buf, uint32_t size);

  void (*_log)(int level, char *tag, const char *fmt, ...);
  void (*_log_raw)(int level, char *tag, const char *fmt, ...);
  uint32_t (*_get_time)(void);

  void (*_bk_printf)(const char *fmt, ...);
  void (*_bk_null_printf)(const char *fmt, ...);
  void (*_shell_set_log_level)(int level);

  void *(*_os_malloc)(uint32_t size);
  void (*_os_free)(void *mem_ptr);
  int32_t (*_os_memcmp)(const void *s, const void *s1, uint32_t n);
  void *(*_os_memset)(void *b, int c, UINT32 len);
  void *(*_os_zalloc)(size_t size);
  UINT32 (*_os_strtoul)(const char *nptr, char **endptr, int base);
  int (*_os_strcasecmp)(const char *s1, const char *s2);
  int32_t (*_os_strcmp)(const char *s1, const char *s2);
  int32_t (*_os_strncmp)(const char *s1, const char *s2, const uint32_t n);
  void *(*_os_memcpy)(void *out, const void *in, uint32_t n);

  void (*_rtos_assert)(uint32_t exp);
  void (*_rtos_enable_int)(uint32_t int_level);
  uint32_t (*_rtos_disable_int)(void);
  uint32_t (*_rtos_get_time)(void);
  int (*_rtos_start_timer)(void *timer);
  int (*_rtos_init_timer)(void *timer, uint32_t time_ms, void *function,
                          void *arg);
  int (*_rtos_deinit_timer)(void *timer);
  bk_err_t (*_rtos_reload_timer)(void *timer);
  int (*_rtos_delay_milliseconds)(uint32_t num_ms);

  /* BK7256 (RISC-V) only; NULL on this part, exactly as the vendor
   * leaves them when CONFIG_SOC_BK7256XX is not set.
   */

  uint64 (*_riscv_get_mtimer)(void);

  void (*_sys_drv_set_ana_vctrl_sysldo)(uint32_t value);
  uint32_t (*_sys_drv_analog_reg3_set)(uint32_t value);
  uint32_t (*_sys_drv_analog_reg2_set)(uint32_t value);
  void (*_sys_drv_flash_cksel)(uint32_t value);
  void (*_sys_drv_set_ana_scal_en)(uint32_t value);
  void (*_sys_drv_set_ana_gadc_buf_ictrl)(uint32_t value);
  void (*_sys_drv_set_ana_gadc_cmp_ictrl)(uint32_t value);
  void (*_sys_drv_set_ana_vtempsel)(uint32_t value);
  void (*_sys_drv_set_ana_vref_sel)(uint32_t value);
  void (*_sys_drv_set_ana_vhsel_ldodig)(uint32_t value);
  void (*_sys_drv_set_ana_pwd_gadc_buf)(uint32_t value);
  uint32_t (*_sys_drv_set_bgcalm)(uint32_t value);
  uint32_t (*_sys_drv_get_bgcalm)(void);
  uint32_t (*_sys_drv_set_vdd_value)(uint32_t value);
  uint32_t (*_sys_drv_get_vdd_value)(void);

  uint32_t (*_aon_pmu_hal_get_reg0x7c)(void);
  uint32_t (*_aon_pmu_get_device_id)(void);

  int (*_gpio_dev_unmap)(uint32_t gpio_id);
  int (*_gpio_dev_map_txen)(uint32_t gpio_id);
  int (*_bk_gpio_pull_down)(uint32_t gpio_id);

  uint32_t (*_bk_get_hardware_chip_id_version)(void);
  int (*_bk_efuse_read_byte)(uint8_t addr, uint8_t *data);

  /* BK7239/BK7286 only; NULL on this part. */

  void (*_sys_ll_set_ana_reg0_cksel)(uint32_t value);
  void (*_sys_ll_set_ana_reg0_dsptrig)(uint32_t value);
  void (*_sys_ll_set_ana_reg8_t_vanaldosel)(uint32_t value);
  void (*_sys_ll_set_ana_reg8_r_vanaldosel)(uint32_t value);
  void (*_sys_ll_set_ana_reg9_vcorelsel)(uint32_t value);

  void (*_bk_set_g_saradc_flag)(UINT8 flag);

  bk_err_t (*_ble_in_dut_mode)(void);
  uint8_t (*_get_tx_pwr_idx)(void);
  void (*_txpwr_max_set_bt_polar)(void);
  void (*_ble_tx_testmode_retrig)(void);

  int (*_gpio_dev_map_rxen)(uint32_t gpio_id);

  uint8_t (*_bk_phy_get_wifi_media_mode_config)(void);
  int (*_bk_feature_save_rfcali_to_otp_enable)(void);
  int (*_bk_otp_ahb_read)(uint32_t item, uint8_t *buf, uint32_t size);
  int (*_bk_otp_ahb_update)(uint32_t item, uint8_t *buf, uint32_t size);
  int (*_bk_get_otp_ahb_rfcali_item)(void);
  void (*_sys_ll_set_ana_reg8_violdosel)(uint32_t value);
  void (*_sys_ll_set_ana_reg8_iocurlim)(uint32_t value);
  int (*_bk_feature_phy_log_enable)(void);
} phy_os_funcs_t;

/* Replica of phy_os_variable_t (bk_phy_adapter.h, lines 193..282).  The
 * chip-id block is the generic branch: not BK7256XX, not BK7239XX/BK7286XX.
 * _cmd_ble_rf_bit_set/_clr are present because this part is not BK7231.
 */

typedef struct
{
  uint32_t _saradc_autotest;

  uint32_t _rf_cfg_tssi_item;
  uint32_t _rf_cfg_mode_item;
  uint32_t _rf_cfg_tssi_b_item;

  uint32_t _cmd_tl410_clk_pwr_up;
  uint32_t _pwd_ble_clk_bit;

  uint32_t _pm_cpu_frq_60m;
  uint32_t _pm_cpu_frq_80m;
  uint32_t _pm_cpu_frq_120m;
  uint32_t _pm_cpu_frq_320m;
  uint32_t _pm_cpu_frq_default;

  uint32_t _pm_dev_id_phy;

  uint32_t _dd_dev_type_ble;
  uint32_t _dd_dev_type_sctrl;
  uint32_t _dd_dev_type_icu;
  uint32_t _dd_dev_type_flash;

  uint32_t _chip_version_a;
  uint32_t _chip_version_b;
  uint32_t _chip_version_c;
  uint32_t _chip_version_default;

  uint32_t _pm_chip_id_mask;
  uint32_t _pm_chip_id_mpw_v2_3;
  uint32_t _pm_chip_id_mpw_v4;
  uint32_t _pm_chip_id_mp_a;

  uint32_t _cmd_get_device_id;
  uint32_t _cmd_sctrl_ble_powerdown;
  uint32_t _cmd_sctrl_ble_powerup;

  uint32_t _cmd_ble_rf_bit_set;
  uint32_t _cmd_ble_rf_bit_clr;

  uint32_t _cmd_sctrl_get_vdd_value;
  uint32_t _cmd_sctrl_set_vdd_value;

  int32_t  _param_xtalh_ctune_mask;
  uint32_t _param_aud_dac_gain_mask;

  uint32_t _pm_power_module_state_on;
  uint32_t _pm_power_module_state_off;
  uint32_t _pm_power_module_state_none;

  uint32_t _adc_temp_10degree_per_dbpwr;
  uint32_t _adc_temp_buffer_size;
  uint32_t _adc_temp_lsb_per_10degree;
  uint32_t _adc_temp_val_min;
  uint32_t _adc_temp_val_max;
  uint32_t _adc_xtal_dist_intial_val;
  uint32_t _adc_temp_dist_intial_val;

  uint32_t _ieee80211_band_2ghz;
  uint32_t _ieee80211_band_5ghz;
  uint32_t _ieee80211_band_6ghz;
  uint32_t _ieee80211_band_60ghz;
  uint32_t _ieee80211_num_bands;

  uint32_t _otp_mac_address;
  uint32_t _otp_vdddig_bandgap;
  uint32_t _otp_dia;
  uint32_t _otp_gadc_temperature;
  uint32_t _otp_sdmadc_calibration;
} phy_os_variable_t;

/* Replica of rf_control_funcs_t / rf_variable_t (bk_rf_adapter.h).  The
 * layout was confirmed against the shipped rf_module_vote_ctrl, which loads
 * table offsets 0/4 (modem clocks), 16/20 (interrupt lock), 32/36 (LDO
 * pulse-skip trim) and 40 (power vote), and variable offsets 0/4 (off/on)
 * and 12 (the RF sub-domain name).
 */

typedef struct
{
  uint32_t (*_sys_drv_modem_bus_clk_ctrl)(bool clk_en);
  uint32_t (*_sys_drv_modem_clk_ctrl)(bool clk_en);
  void (*_phy_exit_dsss_only)(void);
  void (*_phy_enter_dsss_only)(void);
  uint32_t (*_rtos_disable_int)(void);
  void (*_rtos_enable_int)(uint32_t int_level);
  void (*_rwnx_cal_mac_sleep_rc_recover)(void);
  void (*_sys_drv_module_power_ctrl)(unsigned int module,
                                     uint32_t power_state);
  void (*_sys_drv_set_ana_reg11_apfms)(uint32_t value);
  void (*_sys_drv_set_ana_reg12_dpfms)(uint32_t value);
  bk_err_t (*_bk_pm_module_vote_power_ctrl)(unsigned int module,
                                            uint32_t power_state);
} rf_control_funcs_t;

typedef struct
{
  uint32_t _pm_power_module_state_off;
  uint32_t _pm_power_module_state_on;
  uint32_t _pm_power_module_name_phy;
  uint32_t _pm_power_module_name_rf;
} rf_variable_t;

/* The library hands _rtos_init_timer a pointer to its own beken_timer_t
 * (bk_idk include/os/os.h): three words, of which 'handle' is the OS
 * cookie.  Replicated here so the cookie can hold a NuttX work item
 * without writing past the block the caller owns.
 */

struct phy_beken_timer_s
{
  void  *handle;
  void (*function)(void *arg);
  void  *arg;
};

struct phy_timer_ctx_s
{
  struct work_s             work;
  struct phy_beken_timer_s *owner;
  uint32_t                  ms;
  bool                      running;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Closed bluetooth controller entries used from the PHY table.  Each was
 * confirmed present in bk_idk components/bk_libs/bk7258/libs with
 * arm-none-eabi-nm before being declared here; nothing is declared on
 * faith, because a missing symbol is a link failure.
 */

extern int  ble_in_dut_mode(void);
extern uint8_t get_tx_pwr_idx(void);
extern void txpwr_max_set_bt_polar(void);
extern void bluetooth_rf_test_mode_retrig(void);

/* Closed PHY entries.  rwnx_cal_mac_sleep_rc_recover lives in libbk_phy's
 * bk7236_cal.c and is already linked; the RF arbiter calls it directly as
 * well as through this table.
 */

extern void rwnx_cal_mac_sleep_rc_recover(void);

/* The closed adapter entry points.  rf_adapter_init takes two arguments,
 * not one: funcs and vars, the same shape as phy_adapter_init.
 */

extern void phy_adapter_init(const void *phy_funcs, const void *phy_vars);
extern void rf_adapter_init(const void *rf_funcs, const void *rf_vars);
extern void rf_cntrl_init(void);

/* Interrupt lock shims shared with the bluetooth controller
 * (bk7258_ble_shim.c); not redefined here.
 */

extern uint32_t rtos_disable_int(void);
extern void rtos_enable_int(uint32_t flags);
extern void delay_us(uint32_t us);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Where the library's per-channel register-set callback is parked.  Kept
 * because the vendor keeps it, and because a pointer the library handed us
 * is worth having visible in a memory dump; nothing in this image calls it
 * (see phy_osi_nv_reg_set_hook).
 */

static void *g_phy_nv_reg_hook;

/* The library's own hook installer and the callback it should carry.  Both
 * are in archives this image already links -- bk_phy_set_nv_reg_hook in
 * libwifi.a, nv_phy_reg_set_by_chan_bw in libcom_phy.a.
 */

extern void bk_phy_set_nv_reg_hook(void *hook);
extern void nv_phy_reg_set_by_chan_bw(void);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: phy_ana_write / phy_ana_field
 *
 * Description:
 *   Analog shadow register access.  The words at PHY_SYS_ANA_REG(n) are not
 *   ordinary registers: a write is pushed to the analog island over an
 *   internal SPI link, and bit n of PHY_SYS_ANA_BUSY stays set until that
 *   transfer drains.  Issuing the next write before it clears loses it, so
 *   every write polls.  Reads come straight back from the shadow and need
 *   no handshake.  Same protocol as aud_ana_write() in bk7258_audio.c.
 *
 ****************************************************************************/

static void phy_ana_write(int n, uint32_t val)
{
  uint32_t budget = 100000;

  putreg32(val, PHY_SYS_ANA_REG(n));

  while ((getreg32(PHY_SYS_ANA_BUSY) & (1u << n)) != 0)
    {
      if (--budget == 0)
        {
          return;
        }
    }
}

static void phy_ana_field(int n, uint32_t pos, uint32_t mask, uint32_t val)
{
  uint32_t reg = getreg32(PHY_SYS_ANA_REG(n));

  reg &= ~(mask << pos);
  reg |= (val & mask) << pos;
  phy_ana_write(n, reg);
}

static uint32_t phy_ana_get_field(int n, uint32_t pos, uint32_t mask)
{
  return (getreg32(PHY_SYS_ANA_REG(n)) >> pos) & mask;
}

/****************************************************************************
 * Name: phy_power_domain_ctrl
 *
 * Description:
 *   Drive one power-domain bit in the system block.  Sub-domain identifiers
 *   fold onto their parent, which is what the SDK's power manager does once
 *   its vote counting settles: the silicon has one gate per parent domain.
 *   Bits are inverted, so clearing powers up.
 *
 ****************************************************************************/

/* Temporary, for the "PHY is off during a scan" measurement.  bkreg shows
 * pwd_wifp_phy set and phy_cken clear at idle, during a scan, and while
 * rxsens spins -- so something switches the radio off and nothing switches
 * it back.  This names the caller instead of inferring it.  The return
 * address is the closed library's call site; resolve it against
 * cmake_out/contest2026_252_board_xts/nuttx.
 */

static void phy_pwr_trace(const char *what, unsigned int module,
                          uint32_t state, void *ra)
{
  syslog(LOG_INFO, "phytrace: %s(%u,%" PRIu32 ") ra=%p\n",
         what, module, state, ra);
}

static int phy_power_domain_ctrl(unsigned int module, uint32_t power_state)
{
  uint32_t bit;

  if (module >= PHY_PWR_SUB_DOMAIN_STRIDE)
    {
      module /= PHY_PWR_SUB_DOMAIN_STRIDE;
    }

  if (module > PHY_PWR_MODULE_LAST)
    {
      return PHY_FAIL;
    }

  if (power_state == PHY_PWR_STATE_NONE)
    {
      return PHY_OK;
    }

  bit = 1u << module;

  modifyreg32(PHY_SYS_POWER,
              power_state == PHY_PWR_STATE_ON ? bit : 0,
              power_state == PHY_PWR_STATE_ON ? 0 : bit);
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_modem_clk_ctrl / phy_modem_bus_clk_ctrl
 *
 * Description:
 *   The modem (PHY) clock gate.  This part has no separate modem bus clock
 *   -- the vendor's own bk7258 sys_hal_modem_bus_clk_ctrl() is an empty
 *   function -- so only the functional gate is real.
 *
 ****************************************************************************/

static uint32_t phy_modem_clk_ctrl(bool clk_en)
{
  phy_pwr_trace("modem_clk", 27, clk_en, __builtin_return_address(0));

  modifyreg32(PHY_SYS_CLK_EN,
              clk_en ? 0 : PHY_CKEN_PHY,
              clk_en ? PHY_CKEN_PHY : 0);
  return PHY_OK;
}

static uint32_t phy_modem_bus_clk_ctrl(bool clk_en)
{
  UNUSED(clk_en);
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_osi_wifi_* / phy_osi_no_wifi_*
 *
 * Description:
 *   Written when Wi-Fi was not in the image: the four register-window getters
 *   returned NULL, which is what the vendor returns with CONFIG_WIFI_ENABLE
 *   off, and the media-mode probe reported "off".  The point of answering
 *   rather than leaving them NULL was that the honest answer to "is Wi-Fi
 *   holding the radio" must be no, never an unmapped call.
 *
 *   In configs/xts Wi-Fi *is* in the image, and the four getters now forward
 *   to the real windows (see the CONFIG_BK7258_WIFI_VENDOR branch below).
 *   phy_osi_wifi_media_mode() still returns 0 -- the vendor's own default
 *   when no media mode has been configured -- which is correct here but is
 *   no longer "Wi-Fi is absent", so do not reason from that premise.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: phy_osi_null_reg_api and the four register-API accessors
 *
 * Description:
 *   phy_adapter_init() does not just take this table -- it calls four of
 *   its entries and stores what they return into g_mpb_funcs_t,
 *   g_crm_funcs_t, g_riu_funcs_t and g_phy_funcs_t.  Those are the PHY's
 *   own register APIs, and the closed calibration code reaches through
 *   them by offset: rwnx_tpc_get_pwridx_by_rate() loads g_phy_funcs_t,
 *   takes the function pointer at +260 and branches to it.
 *
 *   Returning NULL here is correct for a BLE-only image -- BLE never
 *   touches those APIs, and Beken's own wrapper returns NULL too when
 *   CONFIG_WIFI_ENABLE is off.  With WiFi in the image it is not: the
 *   stored NULL turns into a branch through [NULL + 260], which arrives as
 *   an instruction access violation inside closed code with nothing naming
 *   the table.
 *
 *   The four real accessors live in libwifi.a.  Nothing else references
 *   them, so naming them here is also what keeps --gc-sections from
 *   collecting them.
 *
 ****************************************************************************/

static void *phy_osi_null_reg_api(void)
{
  return NULL;
}

#ifdef CONFIG_BK7258_WIFI_VENDOR
extern void *mpb_reg_api(void);
extern void *crm_reg_api(void);
extern void *riu_reg_api(void);
extern void *mix_funcs(void);

static void *phy_osi_mpb_reg_api(void)
{
  return mpb_reg_api();
}

static void *phy_osi_crm_reg_api(void)
{
  return crm_reg_api();
}

static void *phy_osi_riu_reg_api(void)
{
  return riu_reg_api();
}

static void *phy_osi_mix_funcs(void)
{
  return mix_funcs();
}

#  define PHY_OSI_MPB_REG_API  phy_osi_mpb_reg_api
#  define PHY_OSI_CRM_REG_API  phy_osi_crm_reg_api
#  define PHY_OSI_RIU_REG_API  phy_osi_riu_reg_api
#  define PHY_OSI_MIX_FUNCS    phy_osi_mix_funcs
#else
#  define PHY_OSI_MPB_REG_API  phy_osi_null_reg_api
#  define PHY_OSI_CRM_REG_API  phy_osi_null_reg_api
#  define PHY_OSI_RIU_REG_API  phy_osi_null_reg_api
#  define PHY_OSI_MIX_FUNCS    phy_osi_null_reg_api
#endif

static uint8_t phy_osi_wifi_media_mode(void)
{
  return 0;
}

/****************************************************************************
 * Name: phy_osi_nv_reg_set_hook
 *
 * Description:
 *   Store the PHY register-set callback the library offers, and that is
 *   the whole implementation.  The vendor binds this to
 *   bk_phy_set_nv_reg_hook(), which lives in libwifi.a and does the same
 *   thing: it parks the pointer so the Wi-Fi NV parameter code can later
 *   ask the PHY to re-apply per-channel, per-bandwidth register values.
 *   No Wi-Fi in this image means no one ever asks, so parking it is not a
 *   stub -- it is the complete behaviour for this configuration.
 *
 *   This entry must not be NULL, and that is not a style preference.  It
 *   is the whole reason bk_cal_if_init took a UsageFault:
 *
 *     nv_init:  ldr r3, [g_phy_funcs_t]
 *               ldr r3, [r3, #44]      @ 0x2c, this entry
 *               bx  r3
 *
 *   calibration_main calls nv_init as its third statement.  A bx loads bit
 *   0 of its target into EPSR.T, so a null pointer clears the Thumb bit
 *   and the core raises INVSTATE instead of executing -- which is why a
 *   null function pointer surfaces here as UFSR.INVSTATE (CFSR 0x00020000)
 *   and not as any kind of memory fault, and why "it is INVSTATE, so it is
 *   not a null pointer" is the wrong inference.  The observed R3 of zero
 *   is the ldr above.
 *
 *   The trap was that the vendor leaves this entry NULL too, under
 *   #if CONFIG_WIFI_ENABLE ... #else in bk_phy_adapter.c, and this port
 *   copied that branch correctly.  What does not carry over is the other
 *   half of the vendor's Wi-Fi-off configuration: with Wi-Fi off the SDK
 *   links libcom_phy.a, whose nv_init is a bare "bx lr" and never reaches
 *   this entry.  This board links libbk_phy.a ahead of it -- the Wi-Fi
 *   build of the same objects -- so the Wi-Fi nv_init is the one that
 *   wins, calling a hook the Wi-Fi-off table never filled in.  Filling it
 *   in is the smaller of the two ways to make those halves agree; the
 *   other is to reorder the archives, which would swap the provider of
 *   every duplicate symbol in them and is not a change to make blind.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: phy_osi_nv_reg_set_hook
 *
 * Description:
 *   Install the library's per-channel register-set callback.
 *
 *   This used to park the pointer in a static nothing reads -- the comment
 *   above g_phy_nv_reg_hook said so out loud.  That is very likely why the
 *   receiver hears nothing: phy_init() and mdm_set_channel() both NULL-check
 *   nv_phy_reg_set_func_ptr and silently skip when it is unset, so the RIU
 *   CCA and packet-detect thresholds are never programmed on any channel.
 *   A radio with no detect threshold scans happily and reports
 *   "recv frame is zero" on every one.
 *
 *   The real installer is bk_phy_set_nv_reg_hook(), which lives in libwifi.a.
 *   Only the WiFi configs link that archive, so the call is gated the same
 *   way as the one in bk7258_phy_adapter_init(); without the gate the nsh
 *   config fails to link, which it did from 5ad4798 until this gate was
 *   added.
 *
 ****************************************************************************/

static void phy_osi_nv_reg_set_hook(void *hook)
{
  g_phy_nv_reg_hook = hook;
#ifdef CONFIG_BK7258_WIFI_VENDOR
  bk_phy_set_nv_reg_hook(hook);
#endif
}

/****************************************************************************
 * Name: phy_osi_get_reset_reason
 *
 * Description:
 *   The vendor returns a boot-cause code latched by its own startup code.
 *   This port does not latch one, and every consumer of it in the library
 *   only distinguishes "cold boot" from "software restart" to decide
 *   whether calibration may be skipped.  Reporting 0 (cold boot) makes it
 *   redo the work, which is the safe direction.
 *
 ****************************************************************************/

static uint32_t phy_osi_get_reset_reason(void)
{
  return 0;
}

/****************************************************************************
 * Name: phy_osi_delay / phy_osi_delay_us
 *
 * Description:
 *   _delay is the vendor's calibrated-by-eye spin (an inner loop of 100 per
 *   count), used inside analog settling sequences that run with interrupts
 *   masked; keeping it a spin rather than a sleep preserves that.
 *
 ****************************************************************************/

static void phy_osi_delay(int32 num)
{
  volatile int32 i;
  volatile int32 j;

  for (i = 0; i < num; i++)
    {
      for (j = 0; j < 100; j++)
        {
        }
    }
}

static void phy_osi_delay_us(UINT32 us)
{
  up_udelay(us);
}

/****************************************************************************
 * Name: phy_osi_ddev_control
 *
 * Description:
 *   Stub.  This is the entry into Beken's legacy driver-model dispatcher,
 *   which the PHY uses only on the pre-BK7236 parts to reach the system
 *   controller; on this part the same work arrives through the dedicated
 *   entries above.  There is no dispatcher in this port, so the handles and
 *   command codes in the variable table are left at zero rather than
 *   guessed -- if this is ever implemented, they must be recomputed from
 *   the SDK's configuration-dependent enums first.
 *
 ****************************************************************************/

static UINT32 phy_osi_ddev_control(UINT32 handle, UINT32 cmd, VOID *param)
{
  UNUSED(handle);
  UNUSED(cmd);
  UNUSED(param);
  return 0;
}

/****************************************************************************
 * Name: phy_osi_wdt_stop
 *
 * Description:
 *   The library asks for this before long calibration runs so a watchdog
 *   reset cannot land in the middle of one.  This port cannot disable the
 *   always-on watchdog without also losing the reset path that recovers a
 *   genuinely hung board, so the closest safe equivalent is taken: re-arm
 *   it, which restarts the full window at exactly the moment the library
 *   says the long operation begins.  Feeding and arming are the same
 *   operation on this part.
 *
 ****************************************************************************/

static int phy_osi_wdt_stop(void)
{
  bk7258_wdt_arm(BK7258_WDT_PERIOD_RUN);
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_osi_sense
 *
 * Description:
 *   One sensor acquisition, the shape both of the vendor's detectors use:
 *   run the converter continuously on one analog channel, take a fixed
 *   batch, drop the leading samples that were taken while the input was
 *   still settling, and average the rest.  Samples of 0 and of the
 *   mid-code are dropped as well -- the vendor discards both, its comment
 *   saying the converter can return 0 in power-save mode.
 *
 *   The average is a raw converter code, not an engineering unit; that is
 *   what the vendor's accessors hand back and what the library expects.
 *
 * Returned Value:
 *   The averaged code, or 0 when the batch produced nothing usable, which
 *   is how the vendor signals the same thing.
 *
 ****************************************************************************/

static uint32_t phy_osi_sense(uint8_t channel, uint8_t saturate)
{
  uint16_t raw[PHY_ADC_TEMP_BUFFER_SIZE];
  uint32_t count = 0;
  uint32_t sum = 0;
  uint32_t i;
  int ret;

  ret = bk7258_saradc_start(channel, PHY_ADC_MODE_CONTINUOUS,
                            bk7258_saradc_div(PHY_ADC_SENSE_CLK),
                            saturate, PHY_ADC_SENSE_STEADY, 0, 0);
  if (ret < 0)
    {
      return 0;
    }

  ret = bk7258_saradc_read(raw, PHY_ADC_TEMP_BUFFER_SIZE,
                           PHY_ADC_SENSE_TIMEOUT_MS);
  bk7258_saradc_stop();

  if (ret < 0)
    {
      return 0;
    }

  for (i = PHY_ADC_SENSE_SKIP; i < PHY_ADC_TEMP_BUFFER_SIZE; i++)
    {
      if (raw[i] != 0 && raw[i] != PHY_ADC_SAMPLE_INVALID)
        {
          sum += raw[i];
          count++;
        }
    }

  return count == 0 ? 0 : sum / count;
}

/****************************************************************************
 * Name: phy_osi_temp_detect_* / phy_osi_get_voltage / phy_osi_temp_enable
 *
 * Description:
 *   Single-shot readings of the on-die temperature sensor and of the
 *   supply, both through the SARADC driver.  Real measurements: the
 *   library's calibration solves for transmit trim from these, and feeding
 *   it constants is how it ends up trimming against a temperature that
 *   never moves.
 *
 *   Each is retried the vendor's three times and range-checked against the
 *   vendor's own validity window before being accepted.  A read that never
 *   lands reports failure and leaves the caller's variable at a nominal
 *   code, so a consumer that ignores the error code still sees a plausible
 *   number -- but the error code is the useful part, because the
 *   library's temperature correction is skipped entirely when this fails,
 *   which is the safe outcome.
 *
 *   Temperature is divided by four before the range check, exactly as the
 *   vendor does for this part; the resulting code is still not degrees.
 *   The code-to-degree anchor lives in factory calibration data this port
 *   cannot reach, so the library will compare this reading against its own
 *   default reference tag rather than against a per-unit one.
 *
 *   _bk_feature_temp_detect_enable still reports the feature disabled.
 *   That flag does not gate the reading above -- calibration calls
 *   _temp_detect_get_temperature directly -- it gates the vendor's
 *   periodic re-trim daemon, and this port runs no such daemon.  Claiming
 *   otherwise would promise drift tracking that does not happen.  The
 *   remaining entries keep mirroring the vendor's disabled-build stubs.
 *
 ****************************************************************************/

static bool phy_osi_temp_detect_is_init(void)
{
  return true;
}

static int phy_osi_temp_detect_init(uint32_t init_temperature)
{
  UNUSED(init_temperature);
  return PHY_OK;
}

static int phy_osi_temp_detect_deinit(void)
{
  return PHY_OK;
}

static int phy_osi_temp_detect_get(uint32_t *temperature)
{
  uint32_t value = 0;
  int retry;

  if (temperature == NULL)
    {
      return PHY_FAIL;
    }

  bk7258_saradc_tempsensor(true);

  for (retry = 0; retry < PHY_ADC_SENSE_RETRY; retry++)
    {
      value = phy_osi_sense(PHY_ADC_TEMP_CHANNEL, PHY_ADC_SAT_MODE_2) >>
              PHY_ADC_TEMP_SHIFT;

      if (value > PHY_ADC_TEMP_VAL_MIN && value < PHY_ADC_TEMP_VAL_MAX)
        {
          break;
        }
    }

  bk7258_saradc_tempsensor(false);

  if (value <= PHY_ADC_TEMP_VAL_MIN || value >= PHY_ADC_TEMP_VAL_MAX)
    {
      *temperature = PHY_TEMP_RAW_NOMINAL;
      return PHY_FAIL;
    }

  *temperature = value;
  return PHY_OK;
}

static int phy_osi_temp_detect_enabled(void)
{
  return 0;
}

static int phy_osi_get_voltage(UINT32 *volt_value)
{
  uint32_t value = 0;
  int retry;

  if (volt_value == NULL)
    {
      return PHY_FAIL;
    }

  for (retry = 0; retry < PHY_ADC_SENSE_RETRY; retry++)
    {
      value = phy_osi_sense(PHY_ADC_VOLT_CHANNEL, PHY_ADC_SAT_MODE_2);
      if (value > PHY_ADC_TEMP_VAL_MIN)
        {
          break;
        }
    }

  if (value <= PHY_ADC_TEMP_VAL_MIN)
    {
      *volt_value = PHY_VOLT_RAW_NOMINAL;
      return PHY_FAIL;
    }

  *volt_value = value;
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_osi_saradc_calculate
 *
 * Description:
 *   Raw SARADC code to volts, the BK7236XX-family formula: the driver's two
 *   calibration codes bracket 1 V and 2 V, so the code maps linearly onto
 *   that interval.  Transcribed from bk_idk driver/saradc/adc_driver.c.
 *   Kept real rather than stubbed because it is pure arithmetic -- it needs
 *   no hardware -- so whatever code the library feeds it converts the same
 *   way it would on the vendor's build.
 *
 ****************************************************************************/

static float phy_osi_saradc_calculate(UINT16 adc_val)
{
  float voltage;

  voltage = (float)((int32_t)adc_val - PHY_SARADC_CODE_1V);
  voltage = voltage / (float)(PHY_SARADC_CODE_2V - PHY_SARADC_CODE_1V) + 1;

  if (voltage < 0)
    {
      voltage = 0.0f;
    }

  return voltage;
}

/****************************************************************************
 * Name: phy_osi_pm_*
 *
 * Description:
 *   Clock and power votes.  The SARADC gate is real: calibration opens it
 *   once up front and expects the converter reachable from then on.  The
 *   CPU-frequency vote is a no-op because this port runs at a fixed clock
 *   and never lowers it -- reporting success keeps the library's own
 *   bookkeeping consistent without promising a frequency change that will
 *   not happen.
 *
 *   The PHY clock and power entries are real: they are on the radio path,
 *   not the sleep path.  The vendor reference-counts these votes in its
 *   power manager.
 *
 *   This used to say "with BLE as the only radio user in this image there is
 *   exactly one voter, so writing the gate directly is equivalent".  That was
 *   true when the file was written and is not true in configs/xts, where Wi-Fi
 *   is a second voter.  It turns out not to matter, but by luck rather than by
 *   design: rf_module_vote_ctrl does its own reference counting one level up,
 *   on a holder bitmask, and only reaches these entries when the mask empties.
 *   Measured on the board -- rwnxl_sleep closes the vote and rwnxl_wakeup
 *   reopens it, and the PHY is powered and clocked for the whole of a scan.
 *
 ****************************************************************************/

static int phy_osi_pm_saradc_pwrup(void)
{
  return bk7258_saradc_pwrup() < 0 ? PHY_FAIL : PHY_OK;
}

static int phy_osi_pm_phy_pwrup(void)
{
  phy_modem_clk_ctrl(true);
  return PHY_OK;
}

static int phy_osi_pm_vote_power_phy(int32_t value)
{
  phy_pwr_trace("vote_phy", PHY_PWR_MODULE_WIFI_PHY, (uint32_t)value,
                __builtin_return_address(0));

  return phy_power_domain_ctrl(PHY_PWR_MODULE_WIFI_PHY, (uint32_t)value);
}

static int phy_osi_pm_vote_cpu_freq(uint32_t module, uint32_t cpu_freq)
{
  UNUSED(module);
  UNUSED(cpu_freq);
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_osi_get_chipid / phy_osi_bias_cal / phy_osi_adc_cal
 *
 * Description:
 *   Die-specific trim latched by the AON PMU at reset.  Real reads: these
 *   are plain always-on registers, they need no driver, and the values pick
 *   the correct silicon-revision workarounds inside the library.  Field
 *   positions are from the SDK's bk7258 aon_pmu_struct.h.
 *
 ****************************************************************************/

static uint32_t phy_osi_get_chipid(void)
{
  return getreg32(PHY_AON_PMU_R7C);
}

static uint32_t phy_osi_bias_cal_get(void)
{
  return (getreg32(PHY_AON_PMU_R7E) >> PHY_AON_R7E_CBCAL_POS) &
         PHY_AON_R7E_CBCAL_MASK;
}

static uint32_t phy_osi_adc_cal_get(void)
{
  return (getreg32(PHY_AON_PMU_R7D) >> PHY_AON_R7D_ADC_CAL_POS) &
         PHY_AON_R7D_ADC_CAL_MASK;
}

/****************************************************************************
 * Name: phy_osi_set_ana_* / phy_osi_get_ana_*
 *
 * Description:
 *   One-line analog shadow accessors, each a direct transcription of the
 *   matching sys_ll_set_ana_regN_* inline from the SDK's bk7258 sys_ll.h.
 *   Every write goes through the busy handshake in phy_ana_field().
 *
 ****************************************************************************/

static void phy_osi_set_adc_div(uint32_t value)
{
  phy_ana_field(PHY_ANA_ADC_DIV, value);
}

static uint32_t phy_osi_get_adc_div(void)
{
  return phy_ana_get_field(PHY_ANA_ADC_DIV);
}

static void phy_osi_set_ioldo_lp(uint32_t value)
{
  phy_ana_field(PHY_ANA_IOLDO_LP, value);
}

/* The driver-level spelling of the same field normalises first: the SDK's
 * sys_hal_set_ioldo_lp() documents 0 as high-power and anything else as
 * low-power, so a caller passing 2 must not land on 0 after masking.
 */

static void phy_osi_drv_set_ioldo_lp(uint32_t value)
{
  phy_ana_field(PHY_ANA_IOLDO_LP, !!value);
}

static void phy_osi_set_vcorehsel(uint32_t value)
{
  phy_ana_field(PHY_ANA_VCOREHSEL, value);
}

static void phy_osi_set_spi_latch1v(uint32_t value)
{
  phy_ana_field(PHY_ANA_SPI_LATCH1V, value);
}

static void phy_osi_set_aldosel(uint32_t value)
{
  phy_ana_field(PHY_ANA_ALDOSEL, value);
}

static void phy_osi_set_dldosel(uint32_t value)
{
  phy_ana_field(PHY_ANA_DLDOSEL, value);
}

static void phy_osi_set_apfms(uint32_t value)
{
  phy_ana_field(PHY_ANA_APFMS, value);
}

static void phy_osi_set_dpfms(uint32_t value)
{
  phy_ana_field(PHY_ANA_DPFMS, value);
}

static void phy_osi_set_cb_cal_manu(uint32_t value)
{
  phy_ana_field(PHY_ANA_BCAL_EN, value);
}

static void phy_osi_set_cb_cal_trig(uint32_t value)
{
  phy_ana_field(PHY_ANA_BCAL_START, value);
}

static void phy_osi_set_cb_cal_manu_val(uint32_t value)
{
  phy_ana_field(PHY_ANA_VBIAS, value);
}

static uint32_t phy_osi_set_xtalh_ctune(uint32_t param)
{
  phy_ana_field(PHY_ANA_XTALH_CTUNE, param);
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_osi_set_iobyapssen / phy_osi_set_violdosel
 *
 * Description:
 *   Both change an IO LDO setting that is latched only while spi_latch1v is
 *   asserted, so the vendor brackets them with that bit and this port does
 *   the same.  The zero case restores the reset-default current limit
 *   rather than clearing the field, again matching the vendor.
 *
 ****************************************************************************/

static void phy_osi_set_iobyapssen(uint32_t value)
{
  phy_ana_field(PHY_ANA_SPI_LATCH1V, 1);
  phy_ana_field(PHY_ANA_IOBYAPSSEN, value ? 1 : 0);
  phy_ana_field(PHY_ANA_SPI_LATCH1V, 0);
}

static void phy_osi_set_violdosel(uint32_t flag)
{
  phy_ana_field(PHY_ANA_SPI_LATCH1V, 1);
  phy_ana_field(PHY_ANA_VIOLDOSEL,
                flag ? PHY_VIOLDOSEL_LIMITED : PHY_VIOLDOSEL_DEFAULT);
  phy_ana_field(PHY_ANA_SPI_LATCH1V, 0);
}

static void phy_osi_set_iocurlim(uint32_t value)
{
  phy_ana_field(PHY_ANA_IOCURLIM, value);
}

/****************************************************************************
 * Name: phy_osi_cali_dpll
 *
 * Description:
 *   Re-run the DPLL band search.  The sequence is the vendor's
 *   sys_drv_cali_dpll(): drop the SPI trigger, wait, raise it again while
 *   the unlock detector is masked, wait for the loop to settle, then unmask
 *   the detector.  The waits are the vendor's two magnitudes -- roughly
 *   10 us and 200 us -- and the whole sequence runs with interrupts masked
 *   so nothing can land between the trigger and the settle.  param selects
 *   the longer pair, which the vendor uses on the resume-from-sleep path.
 *
 ****************************************************************************/

static uint32_t phy_osi_cali_dpll(uint32_t param)
{
  irqstate_t flags = up_irq_save();

  phy_ana_field(PHY_ANA_SPITRIG, 0);
  up_udelay(param ? 20 : 10);

  phy_ana_field(PHY_ANA_SPITRIG, 1);
  phy_ana_field(PHY_ANA_SPIDETEN, 0);
  up_udelay(param ? 340 : 200);

  phy_ana_field(PHY_ANA_SPIDETEN, 1);

  up_irq_restore(flags);
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_osi_set_bgcalm / phy_osi_get_bgcalm
 *
 * Description:
 *   Bandgap trim.  Writing it needs the 1 V latch held, reading it does
 *   not; both follow the SDK's sys_hal_*_bgcalm().
 *
 ****************************************************************************/

static uint32_t phy_osi_set_bgcalm(uint32_t value)
{
  phy_ana_field(PHY_ANA_SPI_LATCH1V, 1);
  phy_ana_field(PHY_ANA_BGCAL, value);
  phy_ana_field(PHY_ANA_SPI_LATCH1V, 0);
  return PHY_OK;
}

static uint32_t phy_osi_get_bgcalm(void)
{
  return phy_ana_get_field(PHY_ANA_BGCAL);
}

/****************************************************************************
 * Name: phy_osi_set_vdd_value / phy_osi_get_vdd_value
 *
 * Description:
 *   Digital core voltage select.  The SDK reaches the same field through
 *   its high-voltage control helper; the register write is identical.
 *
 ****************************************************************************/

static uint32_t phy_osi_set_vdd_value(uint32_t value)
{
  phy_ana_field(PHY_ANA_VCOREHSEL, value);
  return PHY_OK;
}

static uint32_t phy_osi_get_vdd_value(void)
{
  return phy_ana_get_field(PHY_ANA_VCOREHSEL);
}

/****************************************************************************
 * Name: phy_osi_modem_bus_clk_on / phy_osi_modem_clk_on
 ****************************************************************************/

static void phy_osi_modem_bus_clk_on(void)
{
  phy_modem_bus_clk_ctrl(true);
}

static void phy_osi_modem_clk_on(void)
{
  phy_modem_clk_ctrl(true);
}

/****************************************************************************
 * Name: phy_osi_adc_read_raw / phy_osi_saradc_*
 *
 * Description:
 *   The converter acquisition the transmit calibration runs on.  It opens
 *   the analog transmit-power detector on channel 8 at 2.6 MHz with the
 *   longest settle, then repeatedly asks for batches of 32 samples and
 *   takes a trimmed mean of each -- so a failed read here does not crash
 *   it, it just yields a detector reading of zero and a transmit trim
 *   solved against nothing.  That is what these entries used to do.
 *
 *   Both channel and clock arrive from the library and are passed through
 *   rather than assumed, and the mode is the vendor's continuous mode with
 *   its saturation mode 3, which is what bk_cal_saradc_start() configures.
 *
 *   The second pair is the vendor's autotest hook.  It is wired to the
 *   same driver for completeness, converting its arguments the way
 *   bk_saradc_start() does, but nothing calls it: _saradc_autotest is 0 in
 *   the variable table below.  Its source-clock argument is ignored for
 *   the same reason the vendor's own autotest path ignores it -- that path
 *   forces the 26 MHz crystal too.
 *
 ****************************************************************************/

static int phy_osi_adc_read_raw(uint16_t *buf, uint32_t size,
                                uint32_t timeout)
{
  return bk7258_saradc_read(buf, size, timeout) < 0 ? PHY_FAIL : PHY_OK;
}

static int phy_osi_cal_saradc_start(int32_t adc_channel, int32_t adc_clk,
                                    int32_t steady_time)
{
  int ret;

  ret = bk7258_saradc_start((uint8_t)adc_channel, PHY_ADC_MODE_CONTINUOUS,
                            bk7258_saradc_div((uint32_t)adc_clk),
                            PHY_ADC_SAT_MODE_3, (uint8_t)steady_time, 0, 0);

  return ret < 0 ? PHY_FAIL : PHY_OK;
}

static int phy_osi_cal_saradc_stop(int32_t adc_channel)
{
  UNUSED(adc_channel);
  return bk7258_saradc_stop() < 0 ? PHY_FAIL : PHY_OK;
}

static int phy_osi_saradc_start(uint8_t adc_channel, uint32_t sample_rate,
                                uint32_t div, uint32_t saradc_clk,
                                uint32_t saradc_mode, uint32_t saradc_xtal,
                                uint32_t saturate_mode,
                                uint32_t steady_time)
{
  int ret;

  UNUSED(saradc_clk);
  UNUSED(saradc_xtal);

  ret = bk7258_saradc_start(adc_channel, (uint8_t)saradc_mode, div,
                            (uint8_t)saturate_mode, (uint8_t)steady_time,
                            (uint8_t)sample_rate, 0);

  return ret < 0 ? PHY_FAIL : PHY_OK;
}

static int phy_osi_saradc_stop(uint8_t adc_channel)
{
  UNUSED(adc_channel);
  return bk7258_saradc_stop() < 0 ? PHY_FAIL : PHY_OK;
}

static void phy_osi_set_saradc_flag(UINT8 flag)
{
  bk7258_saradc_set_flag(flag);
}

/****************************************************************************
 * Name: phy_osi_flash_* / phy_osi_otp_*
 *
 * Description:
 *   Every one of these reports failure, and that is the port-wide decision
 *   about factory calibration made concrete.
 *
 *   The NuttX image is flashed at 0x11000; the RF calibration partition
 *   the vendor writes at production test sits at 0x7fe000, and this port
 *   carries no flash controller driver -- the execute-in-place window is
 *   CRC-encoded and cannot be read as a plain data region, so the bytes are
 *   not reachable even for reading.  OTP is likewise not driven here.
 *
 *   Failing rather than returning zeroed buffers is the whole point: a
 *   zero-filled "successful" read looks like a valid calibration record
 *   with every trim at zero, whereas a failure makes the library fall back
 *   to manual_cal_load_default_txpwr_tab() and the board tables in
 *   bk7258_vnd_cal.c.  Transmit power then comes out nominal instead of
 *   per-unit trimmed -- not accurate, but transmitting.
 *
 *   The partition locator returns 0 for the same reason: there is no
 *   partition table, and 0 is the address the library treats as "absent"
 *   rather than a plausible-looking offset it would then try to read.
 *
 ****************************************************************************/

static int phy_osi_flash_read_bytes(uint32_t address, uint8_t *user_buf,
                                    uint32_t size)
{
  UNUSED(address);
  UNUSED(user_buf);
  UNUSED(size);
  return PHY_FAIL;
}

static int phy_osi_flash_erase_sector(uint32_t address)
{
  UNUSED(address);
  return PHY_FAIL;
}

static int phy_osi_flash_write_bytes(uint32_t address,
                                     const uint8_t *user_buf, uint32_t size)
{
  UNUSED(address);
  UNUSED(user_buf);
  UNUSED(size);
  return PHY_FAIL;
}

static bk_err_t phy_osi_flash_protect_none(void)
{
  return PHY_FAIL;
}

static bk_err_t phy_osi_flash_unprotect_last_block(void)
{
  return PHY_FAIL;
}

static uint32_t phy_osi_flash_rf_firmware_info(void)
{
  return 0;
}

static int phy_osi_otp_read(uint32_t item, uint8_t *buf, uint32_t size)
{
  UNUSED(item);
  UNUSED(buf);
  UNUSED(size);
  return PHY_FAIL;
}

static int phy_osi_otp_update(uint32_t item, uint8_t *buf, uint32_t size)
{
  UNUSED(item);
  UNUSED(buf);
  UNUSED(size);
  return PHY_FAIL;
}

static int phy_osi_otp_rfcali_item(void)
{
  return PHY_FAIL;
}

static int phy_osi_save_rfcali_to_otp_enabled(void)
{
  return 0;
}

/****************************************************************************
 * Name: phy_osi_log / phy_osi_printf
 *
 * Description:
 *   The vendor points these at its own console writer, whose level values
 *   follow components/log.h (1 error .. 4 debug).  The tag is dropped
 *   rather than composed into a scratch buffer: calibration logging can be
 *   reached with interrupts masked, where a stack buffer large enough for a
 *   formatted line is not something to spend.
 *
 ****************************************************************************/

static void phy_osi_log(int level, char *tag, const char *fmt, ...)
{
  va_list ap;
  int priority;

  UNUSED(tag);

  switch (level)
    {
      case 1:
        priority = LOG_ERR;
        break;

      case 2:
        priority = LOG_WARNING;
        break;

      case 3:
        priority = LOG_INFO;
        break;

      default:
        priority = LOG_DEBUG;
        break;
    }

  va_start(ap, fmt);
  vsyslog(priority, fmt, ap);
  va_end(ap);
}

static void phy_osi_printf(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsyslog(LOG_INFO, fmt, ap);
  va_end(ap);
}

static void phy_osi_null_printf(const char *fmt, ...)
{
  UNUSED(fmt);
}

static void phy_osi_shell_set_log_level(int level)
{
  UNUSED(level);
}

/****************************************************************************
 * Name: phy_osi_phy_log_enabled
 *
 * Description:
 *   Reports the library's own trace off, matching the vendor's shipped
 *   bk7258 configuration (CONFIG_PHY_LOG_ENABLE=n).  Returning 1 here is
 *   the switch that turns the closed library's internal tracing on through
 *   _log above; it costs nothing at link time, but the trace is emitted
 *   from inside calibration loops, so it is off by default.
 *
 ****************************************************************************/

static int phy_osi_phy_log_enabled(void)
{
  /* On for bring-up.  This gate is the closed PHY library's own narration --
   * calibration steps, RF configuration, and every manual_cal_* flash
   * diagnostic.  With it off those messages are not merely quiet, they are
   * suppressed at every syslog level, which is why the flash calibration
   * failures were invisible until they were reasoned out.
   *
   * Turn it back off once the receiver works; it is chatty.
   */

  return 1;
}

/****************************************************************************
 * Name: phy_osi_ate_is_enabled
 *
 * Description:
 *   Reports production-test mode off, as bk7258_bt_osi.c does for the same
 *   question.  The vendor samples a boot pin latch this port does not read,
 *   and off is the state a shipped board is in.
 *
 ****************************************************************************/

static bool phy_osi_ate_is_enabled(void)
{
  return false;
}

/****************************************************************************
 * Name: phy_osi_malloc / free / mem and string helpers
 ****************************************************************************/

static void *phy_osi_malloc(uint32_t size)
{
  return kmm_malloc(size);
}

static void phy_osi_free(void *mem_ptr)
{
  kmm_free(mem_ptr);
}

static void *phy_osi_zalloc(size_t size)
{
  return kmm_zalloc(size);
}

static int32_t phy_osi_memcmp(const void *s, const void *s1, uint32_t n)
{
  return memcmp(s, s1, n);
}

static void *phy_osi_memset(void *b, int c, UINT32 len)
{
  return memset(b, c, len);
}

static void *phy_osi_memcpy(void *out, const void *in, uint32_t n)
{
  return memcpy(out, in, n);
}

static UINT32 phy_osi_strtoul(const char *nptr, char **endptr, int base)
{
  return (UINT32)strtoul(nptr, endptr, base);
}

static int phy_osi_strcasecmp(const char *s1, const char *s2)
{
  return strcasecmp(s1, s2);
}

static int32_t phy_osi_strcmp(const char *s1, const char *s2)
{
  return strcmp(s1, s2);
}

static int32_t phy_osi_strncmp(const char *s1, const char *s2,
                               const uint32_t n)
{
  return strncmp(s1, s2, n);
}

/****************************************************************************
 * Name: phy_osi_assert
 *
 * Description:
 *   The vendor panics here.  During bring-up a panic destroys the evidence
 *   that would say why the library gave up, so this logs and returns, the
 *   same choice bk7258_bt_osi.c made for the controller's reboot request.
 *
 ****************************************************************************/

static void phy_osi_assert(uint32_t exp)
{
  if (exp == 0)
    {
      syslog(LOG_ERR, "phy_osi: library assertion failed\n");
    }
}

/****************************************************************************
 * Name: phy_osi_get_time / phy_osi_delay_milliseconds
 *
 * Description:
 *   Milliseconds since boot.  The system tick is 10 ms here
 *   (CONFIG_USEC_PER_TICK=10000), so this has 10 ms granularity; every
 *   consumer in the library uses it for coarse timeouts and elapsed-time
 *   checks, not for RF timing, which comes off hardware counters.
 *
 *   The delay is a busy wait rather than a sleep for the same reason as in
 *   the controller port: it is called from analog settling paths that may
 *   already hold an interrupt lock, where a sleep would either assert or
 *   never wake.
 *
 ****************************************************************************/

static uint32_t phy_osi_get_time(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

static int phy_osi_delay_milliseconds(uint32_t num_ms)
{
  up_mdelay(num_ms);
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_osi_timer_worker / phy_osi_*_timer
 *
 * Description:
 *   The library owns the beken_timer_t block and passes it by pointer, so
 *   only its three words may be touched; the NuttX work item lives in a
 *   separate allocation hung off the block's 'handle' word, which is what
 *   that word is for.
 *
 *   Callbacks run on the high-priority work queue rather than a watchdog:
 *   the vendor's are FreeRTOS software timers, so their handlers execute in
 *   a task and are free to allocate or take a lock, none of which is legal
 *   in a NuttX wdog callback.  As in the controller port, the timer re-arms
 *   before invoking the handler so the period is measured from callback
 *   entry, matching the vendor's own reload-then-call order.  A period
 *   shorter than one tick is clamped to one tick, since the tick here is
 *   10 ms and a zero-delay work item would spin the queue.
 *
 ****************************************************************************/

static void phy_osi_timer_worker(void *arg)
{
  struct phy_timer_ctx_s *ctx = arg;
  struct phy_beken_timer_s *t = ctx->owner;
  clock_t delay = MSEC2TICK(ctx->ms);

  if (ctx->running)
    {
      work_queue(HPWORK, &ctx->work, phy_osi_timer_worker, ctx,
                 delay > 0 ? delay : 1);
    }

  if (t != NULL && t->function != NULL)
    {
      t->function(t->arg);
    }
}

static int phy_osi_init_timer(void *timer, uint32_t time_ms,
                              void *function, void *arg)
{
  struct phy_beken_timer_s *t = timer;
  struct phy_timer_ctx_s *ctx;

  if (t == NULL)
    {
      return PHY_FAIL;
    }

  ctx = kmm_zalloc(sizeof(struct phy_timer_ctx_s));
  if (ctx == NULL)
    {
      return PHY_FAIL;
    }

  ctx->owner   = t;
  ctx->ms      = time_ms;

  t->function  = (void (*)(void *))function;
  t->arg       = arg;
  t->handle    = ctx;
  return PHY_OK;
}

static int phy_osi_start_timer(void *timer)
{
  struct phy_beken_timer_s *t = timer;
  struct phy_timer_ctx_s *ctx;
  clock_t delay;

  if (t == NULL || t->handle == NULL)
    {
      return PHY_FAIL;
    }

  ctx = t->handle;
  ctx->running = true;
  delay = MSEC2TICK(ctx->ms);

  return work_queue(HPWORK, &ctx->work, phy_osi_timer_worker, ctx,
                    delay > 0 ? delay : 1) < 0 ? PHY_FAIL : PHY_OK;
}

static bk_err_t phy_osi_reload_timer(void *timer)
{
  return phy_osi_start_timer(timer);
}

static int phy_osi_deinit_timer(void *timer)
{
  struct phy_beken_timer_s *t = timer;
  struct phy_timer_ctx_s *ctx;

  if (t == NULL || t->handle == NULL)
    {
      return PHY_FAIL;
    }

  ctx = t->handle;
  ctx->running = false;

  /* Synchronous cancel, so the worker cannot still be mid-callback on a
   * block that is about to be freed -- but not from interrupt context,
   * where the sync path would block on the very work thread it interrupted.
   */

  if (up_interrupt_context())
    {
      work_cancel(HPWORK, &ctx->work);
    }
  else
    {
      work_cancel_sync(HPWORK, &ctx->work);
    }

  t->handle = NULL;
  kmm_free(ctx);
  return PHY_OK;
}

/****************************************************************************
 * Name: phy_osi_gpio_dev_map_rxen
 *
 * Description:
 *   Fails, for the same reason bt_osi_gpio_dev_map() does: the caller names
 *   a pad by GPIO number and asks for the external RX-switch function on
 *   it, and turning that into this part's per-pin alternate-function index
 *   needs the SDK's gpio_map table, which this port does not carry.  This
 *   board has no external RX switch either, so there is nothing to route;
 *   picking an alternate function at random would put a live peripheral on
 *   an arbitrary pad.
 *
 ****************************************************************************/

static int phy_osi_gpio_dev_map_rxen(uint32_t gpio_id)
{
  UNUSED(gpio_id);
  return PHY_FAIL;
}

/****************************************************************************
 * Name: phy_osi_ble_in_dut_mode / _get_tx_pwr_idx / _txpwr_max_set_bt_polar
 *        / _ble_tx_testmode_retrig
 *
 * Description:
 *   Forwarded to the closed bluetooth controller, which is where the state
 *   they report actually lives: the DUT-mode flag, the negotiated transmit
 *   power index, the polar-mode power ceiling and the test-mode retrigger.
 *   Stubbing _get_tx_pwr_idx in particular would pin transmit power to
 *   index 0 regardless of what the link layer asked for.
 *
 ****************************************************************************/

static bk_err_t phy_osi_ble_in_dut_mode(void)
{
  return ble_in_dut_mode();
}

static uint8_t phy_osi_get_tx_pwr_idx(void)
{
  return get_tx_pwr_idx();
}

static void phy_osi_txpwr_max_set_bt_polar(void)
{
  txpwr_max_set_bt_polar();
}

static void phy_osi_ble_tx_testmode_retrig(void)
{
  bluetooth_rf_test_mode_retrig();
}

/****************************************************************************
 * Name: phy_osi_rf_module_power_ctrl / phy_osi_rf_pm_vote_power
 *
 * Description:
 *   The two power entries the RF arbiter uses.  rf_module_vote_ctrl calls
 *   the vote form with the RF sub-domain name on every open and close of
 *   the radio, so this is on the live path, not the sleep path, and it does
 *   real work.  The direct form is the vendor's un-counted variant; both
 *   land on the same domain gate here because this image has a single
 *   radio user and therefore a vote count that is always 0 or 1.
 *
 ****************************************************************************/

static void phy_osi_rf_module_power_ctrl(unsigned int module,
                                         uint32_t power_state)
{
  phy_pwr_trace("rf_module", module, power_state,
                __builtin_return_address(0));

  phy_power_domain_ctrl(module, power_state);
}

static bk_err_t phy_osi_rf_pm_vote_power(unsigned int module,
                                         uint32_t power_state)
{
  phy_pwr_trace("rf_vote", module, power_state,
                __builtin_return_address(0));

  return phy_power_domain_ctrl(module, power_state);
}

/****************************************************************************
 * Name: phy_osi_dsss_only_stub
 *
 * Description:
 *   Stub pair.  Both switch the Wi-Fi modem between DSSS-only and full
 *   OFDM reception while bluetooth holds the radio.
 *
 *   The original reason given here -- "no Wi-Fi modem in this image, and the
 *   vendor compiles its bodies out under the same condition" -- is wrong for
 *   configs/xts.  bk_rf_adapter.c:27-46 guards them on CONFIG_WIFI_ENABLE,
 *   which is on, so the vendor calls the real phy_exit_dsss_only() /
 *   phy_enter_dsss_only() from libwifi.a (phy_karst_bk7236.c.obj).
 *
 *   Stubbing them is nevertheless harmless, for a different reason, and this
 *   one is checked rather than assumed -- disassembled out of the archive:
 *   both bodies are gated on dsss_only_flag, which lives in .bss and is set
 *   only by phy_enter_dsss_only itself.  At boot the flag is 0, so the real
 *   phy_exit_dsss_only returns without touching the modem, and the only
 *   thing our stub skips on the enter side is setting RIU 0x4980b390 bit 9.
 *   Net effect: this port never narrows the receiver to DSSS, where the
 *   vendor would while BLE holds the radio.  That is more receive capability,
 *   not less, so it is not a candidate for "recv frame is zero".
 *
 *   Wire them for real before shipping Wi-Fi/BLE coexistence, where the
 *   narrowing is the point.
 *
 ****************************************************************************/

static void phy_osi_dsss_only_stub(void)
{
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Not const: the library keeps only the pointers it is handed, and the
 * vendor's own copies live in writable data.  Entries left NULL are the
 * ones the vendor also leaves NULL for this part -- the BK7256 RISC-V
 * block, the BK7239 analog block, and the Wi-Fi test hooks -- and an empty
 * stub there would be worse than NULL, because several of them are feature
 * probes whose honest answer is "absent", not "present and does nothing".
 */

/****************************************************************************
 * Rate-sensitivity and EVM entries
 *
 * These were NULL, justified by "absent, as the vendor leaves them with
 * CONFIG_WIFI_ENABLE off".  That premise is false for this build:
 * sdkconfig.h on the vendor include path has CONFIG_WIFI_ENABLE 1, and
 * Beken's own g_phy_os_funcs assigns all eight unconditionally, with no
 * wrapper and no guard.
 *
 * Leaving them NULL is not merely incomplete, it is eight armed traps: the
 * consumers do "ldr r3,[rX,#off]; blx r3" with no NULL test, and rxsens
 * (do_rx_sensitivity) and txevm (do_evm) are live registered console
 * commands in this image.
 *
 * They are wired now for a second reason.  The receiver currently reports
 * zero frames on all thirteen channels and takes no RX-trigger interrupt at
 * all; rxsens is the vendor's own receiver test, and it is a far more
 * direct instrument than reading more disassembly.
 *
 * Note _evm_init takes evm_phy_init -- the field name and the symbol name
 * do not match, and there is no symbol called evm_init.  Note also that
 * _evm_set_ke_evt_mac_bit and _evm_clear_ke_evt_mac_bit must be wired
 * together; one without the other arms half of the KE_EVT_MAC path.
 *
 * All eight live in libwifi.a, already linked.
 *
 ****************************************************************************/

#ifdef CONFIG_BK7258_WIFI_VENDOR
extern void rs_init(uint32_t channel, int32_t band, uint32_t mode);
extern void rs_bypass_mac_init(uint32_t channel, int32_t band,
                               uint32_t mode);
extern void rs_deinit(void);
extern void evm_phy_init(uint32_t channel, int32_t band, uint32_t bw);
extern void evm_bypass_mac_init(uint32_t freq, int32_t band, uint32_t bw);
extern void evm_clear_ke_evt_mac_bit(void);
extern void evm_set_ke_evt_mac_bit(void);
extern void tx_evm_set_chan_ctxt_pop(void *chan_info);

#  define PHY_OSI_RS_INIT            rs_init
#  define PHY_OSI_RS_BYPASS_MAC_INIT rs_bypass_mac_init
#  define PHY_OSI_RS_DEINIT          rs_deinit
#  define PHY_OSI_EVM_INIT           evm_phy_init
#  define PHY_OSI_EVM_BYPASS_MAC     evm_bypass_mac_init
#  define PHY_OSI_EVM_CLR_KE_EVT     evm_clear_ke_evt_mac_bit
#  define PHY_OSI_EVM_SET_KE_EVT     evm_set_ke_evt_mac_bit
#  define PHY_OSI_TX_EVM_CHAN_POP    tx_evm_set_chan_ctxt_pop
#else
#  define PHY_OSI_RS_INIT            NULL
#  define PHY_OSI_RS_BYPASS_MAC_INIT NULL
#  define PHY_OSI_RS_DEINIT          NULL
#  define PHY_OSI_EVM_INIT           NULL
#  define PHY_OSI_EVM_BYPASS_MAC     NULL
#  define PHY_OSI_EVM_CLR_KE_EVT     NULL
#  define PHY_OSI_EVM_SET_KE_EVT     NULL
#  define PHY_OSI_TX_EVM_CHAN_POP    NULL
#endif

phy_os_funcs_t g_phy_os_funcs =
{
  ._version                          = PHY_OSI_VERSION,

  ._mpb_reg_api                      = PHY_OSI_MPB_REG_API,
  ._crm_reg_api                      = PHY_OSI_CRM_REG_API,
  ._riu_reg_api                      = PHY_OSI_RIU_REG_API,
  ._mix_funcs                        = PHY_OSI_MIX_FUNCS,
  ._bk_misc_get_reset_reason         = phy_osi_get_reset_reason,

  /* Wi-Fi rate-sensitivity and EVM test entries: absent, as the vendor
   * leaves them with CONFIG_WIFI_ENABLE off.  Verified absent-and-safe
   * rather than assumed: with --gc-sections applied, the only code left in
   * the image that reaches any of them is nv_init, and it reaches
   * _nv_phy_reg_set_hook, which is therefore the one entry here that is
   * filled in.
   */

  ._rs_init                          = PHY_OSI_RS_INIT,
  ._rs_bypass_mac_init               = PHY_OSI_RS_BYPASS_MAC_INIT,
  ._rs_deinit                        = PHY_OSI_RS_DEINIT,
  ._evm_init                         = PHY_OSI_EVM_INIT,
  ._evm_bypass_mac_init              = PHY_OSI_EVM_BYPASS_MAC,
  ._nv_phy_reg_set_hook              = phy_osi_nv_reg_set_hook,
  ._evm_clear_ke_evt_mac_bit         = PHY_OSI_EVM_CLR_KE_EVT,
  ._evm_set_ke_evt_mac_bit           = PHY_OSI_EVM_SET_KE_EVT,
  ._tx_evm_set_chan_ctxt_pop         = PHY_OSI_TX_EVM_CHAN_POP,
  ._save_info_item                   = NULL,
  ._get_info_item                    = NULL,

  ._delay                            = phy_osi_delay,
  ._delay_us                         = phy_osi_delay_us,
  ._ddev_control                     = phy_osi_ddev_control,
  ._bk_wdt_stop                      = phy_osi_wdt_stop,

  ._temp_detect_is_init              = phy_osi_temp_detect_is_init,
  ._temp_detect_deinit               = phy_osi_temp_detect_deinit,
  ._temp_detect_init                 = phy_osi_temp_detect_init,
  ._temp_detect_get_temperature      = phy_osi_temp_detect_get,
  ._bk_feature_temp_detect_enable    = phy_osi_temp_detect_enabled,

  ._ate_is_enabled                   = phy_osi_ate_is_enabled,
  ._volt_single_get_current_voltage  = phy_osi_get_voltage,
  ._saradc_calculate                 = phy_osi_saradc_calculate,

  ._bk_pm_clock_ctrl_saradc_pwrup    = phy_osi_pm_saradc_pwrup,
  ._bk_pm_clock_ctrl_phy_pwrup       = phy_osi_pm_phy_pwrup,
  ._bk_pm_module_vote_power_ctrl_phy = phy_osi_pm_vote_power_phy,
  ._bk_pm_module_vote_cpu_freq       = phy_osi_pm_vote_cpu_freq,

  ._aon_pmu_hal_get_chipid           = phy_osi_get_chipid,
  ._aon_pmu_drv_bias_cal_get         = phy_osi_bias_cal_get,
  ._aon_pmu_drv_get_adc_cal          = phy_osi_adc_cal_get,

  ._sys_ll_set_ana_reg5_adc_div      = phy_osi_set_adc_div,
  ._sys_ll_get_ana_reg5_adc_div      = phy_osi_get_adc_div,
  ._sys_ll_set_ana_reg8_ioldo_lp     = phy_osi_set_ioldo_lp,
  ._sys_ll_set_ana_reg9_vcorehsel    = phy_osi_set_vcorehsel,
  ._sys_ll_set_ana_reg9_spi_latch1v  = phy_osi_set_spi_latch1v,
  ._sys_ll_set_ana_reg10_iobyapssen  = phy_osi_set_iobyapssen,
  ._sys_ll_set_ana_reg11_aldosel     = phy_osi_set_aldosel,
  ._sys_ll_set_ana_reg12_dldosel     = phy_osi_set_dldosel,

  ._sys_drv_cali_dpll                = phy_osi_cali_dpll,
  ._sys_drv_set_ana_ioldo_lp         = phy_osi_drv_set_ioldo_lp,
  ._sys_drv_set_ana_cb_cal_manu_val  = phy_osi_set_cb_cal_manu_val,
  ._sys_drv_set_ana_cb_cal_trig      = phy_osi_set_cb_cal_trig,
  ._sys_drv_set_ana_cb_cal_manu      = phy_osi_set_cb_cal_manu,
  ._sys_drv_analog_set_xtalh_ctune   = phy_osi_set_xtalh_ctune,

  ._phy_sys_drv_modem_bus_clk_ctrl_on = phy_osi_modem_bus_clk_on,
  ._phy_sys_drv_modem_clk_ctrl_on     = phy_osi_modem_clk_on,

  ._bk_adc_read_raw                  = phy_osi_adc_read_raw,
  ._bk_cal_saradc_start              = phy_osi_cal_saradc_start,
  ._bk_cal_saradc_stop               = phy_osi_cal_saradc_stop,
  ._bk_saradc_start                  = phy_osi_saradc_start,
  ._bk_saradc_stop                   = phy_osi_saradc_stop,

  ._bk_flash_read_bytes              = phy_osi_flash_read_bytes,
  ._bk_flash_erase_sector            = phy_osi_flash_erase_sector,
  ._bk_flash_write_bytes             = phy_osi_flash_write_bytes,

  ._bk_flash_set_protect_type_protect_none = phy_osi_flash_protect_none,
  ._bk_flash_set_protect_type_unprotect_last_block =
                                       phy_osi_flash_unprotect_last_block,
  ._bk_flash_partition_get_rf_firmware_info =
                                       phy_osi_flash_rf_firmware_info,

  ._bk_otp_apb_read                  = phy_osi_otp_read,
  ._bk_otp_apb_update                = phy_osi_otp_update,

  ._log                              = phy_osi_log,
  ._log_raw                          = phy_osi_log,
  ._get_time                         = phy_osi_get_time,

  ._bk_printf                        = phy_osi_printf,
  ._bk_null_printf                   = phy_osi_null_printf,
  ._shell_set_log_level              = phy_osi_shell_set_log_level,

  ._os_malloc                        = phy_osi_malloc,
  ._os_free                          = phy_osi_free,
  ._os_memcmp                        = phy_osi_memcmp,
  ._os_memset                        = phy_osi_memset,
  ._os_zalloc                        = phy_osi_zalloc,
  ._os_strtoul                       = phy_osi_strtoul,
  ._os_strcasecmp                    = phy_osi_strcasecmp,
  ._os_strcmp                        = phy_osi_strcmp,
  ._os_strncmp                       = phy_osi_strncmp,
  ._os_memcpy                        = phy_osi_memcpy,

  ._rtos_assert                      = phy_osi_assert,
  ._rtos_enable_int                  = rtos_enable_int,
  ._rtos_disable_int                 = rtos_disable_int,
  ._rtos_get_time                    = phy_osi_get_time,
  ._rtos_start_timer                 = phy_osi_start_timer,
  ._rtos_init_timer                  = phy_osi_init_timer,
  ._rtos_deinit_timer                = phy_osi_deinit_timer,
  ._rtos_reload_timer                = phy_osi_reload_timer,
  ._rtos_delay_milliseconds          = phy_osi_delay_milliseconds,

  /* BK7256 (RISC-V) block: absent on this part. */

  ._riscv_get_mtimer                 = NULL,
  ._sys_drv_set_ana_vctrl_sysldo     = NULL,
  ._sys_drv_analog_reg3_set          = NULL,
  ._sys_drv_analog_reg2_set          = NULL,
  ._sys_drv_flash_cksel              = NULL,
  ._sys_drv_set_ana_scal_en          = NULL,
  ._sys_drv_set_ana_gadc_buf_ictrl   = NULL,
  ._sys_drv_set_ana_gadc_cmp_ictrl   = NULL,
  ._sys_drv_set_ana_vtempsel         = NULL,
  ._sys_drv_set_ana_vref_sel         = NULL,
  ._sys_drv_set_ana_vhsel_ldodig     = NULL,
  ._sys_drv_set_ana_pwd_gadc_buf     = NULL,

  ._sys_drv_set_bgcalm               = phy_osi_set_bgcalm,
  ._sys_drv_get_bgcalm               = phy_osi_get_bgcalm,
  ._sys_drv_set_vdd_value            = phy_osi_set_vdd_value,
  ._sys_drv_get_vdd_value            = phy_osi_get_vdd_value,

  ._aon_pmu_hal_get_reg0x7c          = NULL,
  ._aon_pmu_get_device_id            = NULL,

  ._gpio_dev_unmap                   = NULL,
  ._gpio_dev_map_txen                = NULL,
  ._bk_gpio_pull_down                = NULL,
  ._bk_get_hardware_chip_id_version  = NULL,
  ._bk_efuse_read_byte               = NULL,

  /* BK7239/BK7286 analog block: absent on this part. */

  ._sys_ll_set_ana_reg0_cksel        = NULL,
  ._sys_ll_set_ana_reg0_dsptrig      = NULL,
  ._sys_ll_set_ana_reg8_t_vanaldosel = NULL,
  ._sys_ll_set_ana_reg8_r_vanaldosel = NULL,
  ._sys_ll_set_ana_reg9_vcorelsel    = NULL,

  ._bk_set_g_saradc_flag             = phy_osi_set_saradc_flag,

  ._ble_in_dut_mode                  = phy_osi_ble_in_dut_mode,
  ._get_tx_pwr_idx                   = phy_osi_get_tx_pwr_idx,
  ._txpwr_max_set_bt_polar           = phy_osi_txpwr_max_set_bt_polar,
  ._ble_tx_testmode_retrig           = phy_osi_ble_tx_testmode_retrig,

  ._gpio_dev_map_rxen                = phy_osi_gpio_dev_map_rxen,

  ._bk_phy_get_wifi_media_mode_config = phy_osi_wifi_media_mode,
  ._bk_feature_save_rfcali_to_otp_enable =
                                       phy_osi_save_rfcali_to_otp_enabled,
  ._bk_otp_ahb_read                  = phy_osi_otp_read,
  ._bk_otp_ahb_update                = phy_osi_otp_update,
  ._bk_get_otp_ahb_rfcali_item       = phy_osi_otp_rfcali_item,
  ._sys_ll_set_ana_reg8_violdosel    = phy_osi_set_violdosel,
  ._sys_ll_set_ana_reg8_iocurlim     = phy_osi_set_iocurlim,
  ._bk_feature_phy_log_enable        = phy_osi_phy_log_enabled,
};

/* Constants the library reads out of the table instead of compiling in, so
 * one binary can serve several parts.  The legacy driver-model handles and
 * command codes are left at zero: _ddev_control above is a stub, so nothing
 * consumes them, and the SDK enums they come from are assembled out of a
 * dozen configuration switches -- a transcribed guess would be a value that
 * looks authoritative and is not.
 */

phy_os_variable_t g_phy_os_variable =
{
  ._saradc_autotest            = 0,

  ._rf_cfg_tssi_item           = PHY_RF_CFG_TSSI_ITEM,
  ._rf_cfg_mode_item           = PHY_RF_CFG_MODE_ITEM,
  ._rf_cfg_tssi_b_item         = PHY_RF_CFG_TSSI_B_ITEM,

  ._cmd_tl410_clk_pwr_up       = 0,
  ._pwd_ble_clk_bit            = (1u << 1),

  ._pm_cpu_frq_60m             = PHY_PM_CPU_FRQ_60M,
  ._pm_cpu_frq_80m             = PHY_PM_CPU_FRQ_80M,
  ._pm_cpu_frq_120m            = PHY_PM_CPU_FRQ_120M,
  ._pm_cpu_frq_320m            = PHY_PM_CPU_FRQ_320M,
  ._pm_cpu_frq_default         = PHY_PM_CPU_FRQ_DEFAULT,

  ._pm_dev_id_phy              = PHY_PM_DEV_ID_PHY,

  ._dd_dev_type_ble            = 0,
  ._dd_dev_type_sctrl          = 0,
  ._dd_dev_type_icu            = 0,
  ._dd_dev_type_flash          = 0,

  ._chip_version_a             = PHY_CHIP_VERSION_A,
  ._chip_version_b             = PHY_CHIP_VERSION_B,
  ._chip_version_c             = PHY_CHIP_VERSION_C,
  ._chip_version_default       = PHY_CHIP_VERSION_DEFAULT,

  ._pm_chip_id_mask            = PHY_CHIP_ID_MASK,
  ._pm_chip_id_mpw_v2_3        = PHY_CHIP_ID_MPW_V2_3,
  ._pm_chip_id_mpw_v4          = PHY_CHIP_ID_MPW_V4,
  ._pm_chip_id_mp_a            = PHY_CHIP_ID_MP_A,

  ._cmd_get_device_id          = 0,
  ._cmd_sctrl_ble_powerdown    = 0,
  ._cmd_sctrl_ble_powerup      = 0,
  ._cmd_ble_rf_bit_set         = 0,
  ._cmd_ble_rf_bit_clr         = 0,
  ._cmd_sctrl_get_vdd_value    = 0,
  ._cmd_sctrl_set_vdd_value    = 0,

  ._param_xtalh_ctune_mask     = PHY_PARAM_XTALH_CTUNE_MASK,
  ._param_aud_dac_gain_mask    = PHY_PARAM_AUD_DAC_GAIN_MASK,

  ._pm_power_module_state_on   = PHY_PWR_STATE_ON,
  ._pm_power_module_state_off  = PHY_PWR_STATE_OFF,
  ._pm_power_module_state_none = PHY_PWR_STATE_NONE,

  ._adc_temp_10degree_per_dbpwr = PHY_ADC_TEMP_10DEG_PER_DBPWR,
  ._adc_temp_buffer_size        = PHY_ADC_TEMP_BUFFER_SIZE,
  ._adc_temp_lsb_per_10degree   = PHY_ADC_TEMP_LSB_PER_10DEG,
  ._adc_temp_val_min            = PHY_ADC_TEMP_VAL_MIN,
  ._adc_temp_val_max            = PHY_ADC_TEMP_VAL_MAX,
  ._adc_xtal_dist_intial_val    = PHY_ADC_XTAL_DIST_INITIAL,
  ._adc_temp_dist_intial_val    = PHY_ADC_TEMP_DIST_INITIAL,

  ._ieee80211_band_2ghz        = 0,
  ._ieee80211_band_5ghz        = 1,
  ._ieee80211_band_6ghz        = 2,
  ._ieee80211_band_60ghz       = 3,
  ._ieee80211_num_bands        = 4,

  ._otp_mac_address            = PHY_OTP_MAC_ADDRESS,
  ._otp_vdddig_bandgap         = PHY_OTP_VDDDIG_BANDGAP,
  ._otp_dia                    = PHY_OTP_DIA,
  ._otp_gadc_temperature       = PHY_OTP_GADC_TEMPERATURE,
  ._otp_sdmadc_calibration     = PHY_OTP_SDMADC_CALIBRATION,
};

/* The RF arbiter's table.  _rwnx_cal_mac_sleep_rc_recover forwards to the
 * closed PHY archive rather than being stubbed: it restores the radio
 * controller's register file after a sleep-induced reset of it, the arbiter
 * calls it directly on one path and through this table on another, and the
 * two must do the same thing.
 */

rf_control_funcs_t g_rf_control_funcs =
{
  ._sys_drv_modem_bus_clk_ctrl    = phy_modem_bus_clk_ctrl,
  ._sys_drv_modem_clk_ctrl        = phy_modem_clk_ctrl,
  ._phy_exit_dsss_only            = phy_osi_dsss_only_stub,
  ._phy_enter_dsss_only           = phy_osi_dsss_only_stub,
  ._rtos_disable_int              = rtos_disable_int,
  ._rtos_enable_int               = rtos_enable_int,
  ._rwnx_cal_mac_sleep_rc_recover = rwnx_cal_mac_sleep_rc_recover,
  ._sys_drv_module_power_ctrl     = phy_osi_rf_module_power_ctrl,
  ._sys_drv_set_ana_reg11_apfms   = phy_osi_set_apfms,
  ._sys_drv_set_ana_reg12_dpfms   = phy_osi_set_dpfms,
  ._bk_pm_module_vote_power_ctrl  = phy_osi_rf_pm_vote_power,
};

/* Domain identifiers, not flags: 10 is the PHY power domain and 202 is its
 * RF sub-domain (10 * 20 + 2, the sub-domain stride from the SDK's bk7258
 * sys_types.h).  The arbiter loads the sub-domain name and one of the two
 * states straight out of this block.
 */

rf_variable_t g_rf_variable =
{
  ._pm_power_module_state_off = PHY_PWR_STATE_OFF,
  ._pm_power_module_state_on  = PHY_PWR_STATE_ON,
  ._pm_power_module_name_phy  = PHY_PWR_MODULE_WIFI_PHY,
  ._pm_power_module_name_rf   = PHY_PWR_MODULE_WIFI_PHY *
                                PHY_PWR_SUB_DOMAIN_STRIDE + 2,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_phy_adapter_init
 *
 * Description:
 *   Hand the OS abstraction tables to the closed PHY archive.  Must run
 *   before anything touches the radio: until it does, every pointer the
 *   library reaches through g_phy_funcs_t is NULL.  The library keeps the
 *   two addresses, so the tables must stay alive for the life of the
 *   image -- they are file-scope objects, which they are.
 *
 * Returned Value:
 *   Always OK; the closed entry point returns void and validates nothing,
 *   so there is no acceptance to report.
 *
 ****************************************************************************/

int bk7258_phy_adapter_init(void)
{
  phy_adapter_init(&g_phy_os_funcs, &g_phy_os_variable);

  /* Install the per-channel register-set callback ourselves rather than
   * waiting for the library to do it.
   *
   * In Beken's own build nv_init() calls back through the table's
   * _nv_phy_reg_set_hook slot.  The nv_init() this image links comes from
   * libcom_phy.a -- the Wi-Fi-OFF variant -- and its whole body is "bx lr",
   * so the slot is never invoked no matter how correctly it is filled.
   * Calling the installer directly is archive-order-neutral and avoids
   * pulling in libbk_phy.a for one function.
   */

#ifdef CONFIG_BK7258_WIFI_VENDOR
  bk_phy_set_nv_reg_hook((void *)nv_phy_reg_set_by_chan_bw);
#endif

  return PHY_OK;
}

/****************************************************************************
 * Name: bk7258_rf_adapter_init
 *
 * Description:
 *   Hand the RF arbiter its tables.  This is the one that closes the bus
 *   fault at rf_module_vote_ctrl: that function's first act is to call
 *   through g_rf_funcs_t for the interrupt lock.
 *
 *   rf_cntrl_init() follows, as it does in the vendor's own
 *   bk_rf_adapter_init().  It is not decoration: it clears the arbiter's
 *   private state and sets its "radio is currently closed" flag, and that
 *   flag is what the first RF_OPEN vote tests to decide whether to power
 *   the PHY domain up.  Left at its zero-initialised value the vote would
 *   run to completion, touch nothing, and leave the radio dark -- a quieter
 *   failure than the crash, and a harder one to see.
 *
 * Returned Value:
 *   Always OK; both closed entry points return void.
 *
 ****************************************************************************/

int bk7258_rf_adapter_init(void)
{
  rf_adapter_init(&g_rf_control_funcs, &g_rf_variable);
  rf_cntrl_init();
  return PHY_OK;
}
