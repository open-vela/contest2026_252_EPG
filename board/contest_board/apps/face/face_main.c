/****************************************************************************
 * apps/face/face_main.c
 *
 * The robot face, all vendor art: NEUTRAL.AVI loops as the idle state
 * and S2 cuts straight to the next expression clip -- pressed at any
 * moment, including mid-clip.  Every frame is a 320x160 MJPEG from the
 * SD card, software-decoded (TJpgDec) and split down the middle onto
 * the two 160x160 panels.  S3 quits.
 *
 * Usage: face [expression-name]   e.g. "face HAPPY" plays one clip;
 *        plain "face" runs the interactive demo.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/input/buttons.h>

#include <sys/mount.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>



/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EYE_XRES   160
#define EYE_YRES   160
#define AVI_W      320

#define CHUNK_MAX  (64 * 1024)

#define BTN_S2     (1u << 1)
#define BTN_S3     (1u << 2)

/* play_avi() outcomes */

#define PLAY_END   0                   /* clip finished naturally */
#define PLAY_QUIT  1                   /* S3: leave the program */
#define PLAY_NEXT  2                   /* S2: cut to the next clip */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *g_exprs[] =
{
  "HAPPY", "CURIOUS", "LOVE", "SURPRISE", "THINKING",
  "SAD", "SLEEPY", "ANGRY"
};

#define NEXPRS (sizeof(g_exprs) / sizeof(g_exprs[0]))

static btn_buttonset_t g_btn_last;
static uint8_t *g_yuyv;
static uint8_t *g_rgbl;
static uint8_t *g_rgbr;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Name: btn_event
 *
 * Description:
 *   Poll the buttons and report rising edges: PLAY_QUIT for S3,
 *   PLAY_NEXT for S2, 0 for nothing new.  Kept as the single reader of
 *   button state so no press is ever swallowed between modes.
 *
 ****************************************************************************/

static int btn_event(int btnfd)
{
  /* Raw GPIO poll (the KEYS-probe path, proven on hardware; the
   * /dev/buttons route silently delivered nothing during playback).
   * Debounced by consecutive frames: a single glitched sample can
   * neither switch nor -- worse -- quit.  power (GPIO12) = next
   * expression at 2 frames; >> (GPIO8) = quit at 3 frames (~135 ms).
   */

  extern bool bk7258_gpio_read(int pin);
  static int s2_cnt;
  static int s3_cnt;
  int ev = 0;

  if (!bk7258_gpio_read(8))
    {
      if (++s3_cnt == 3)
        {
          ev = PLAY_QUIT;
        }
    }
  else
    {
      s3_cnt = 0;
    }

  if (!bk7258_gpio_read(12))
    {
      if (++s2_cnt == 2 && ev == 0)
        {
          ev = PLAY_NEXT;
        }
    }
  else
    {
      s2_cnt = 0;
    }

  return ev;
}

/****************************************************************************
 * Name: play_avi
 *
 * Description:
 *   Stream one MJPEG AVI onto both panels, checking the buttons after
 *   every frame.  Returns PLAY_END / PLAY_QUIT / PLAY_NEXT, or a
 *   negative value when the file cannot be played at all.
 *
 ****************************************************************************/

static int play_avi(const char *name, int btnfd)
{
  char path[48];
  uint8_t *chunk = NULL;
  int fd;
  int ret = PLAY_END;
  uint32_t hdr[3];

  snprintf(path, sizeof(path), "/mnt/%s.AVI", name);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }

  chunk = malloc(CHUNK_MAX);
  if (chunk == NULL)
    {
      ret = -1;
      goto out;
    }

  if (read(fd, hdr, 12) != 12)
    {
      ret = -1;
      goto out;
    }

  for (; ; )
    {
      uint32_t fcc;
      uint32_t size;

      if (read(fd, hdr, 8) != 8)
        {
          goto out;                    /* natural end of clip */
        }

      fcc  = hdr[0];
      size = hdr[1];

      if (fcc == 0x5453494c)           /* "LIST" */
        {
          uint32_t ltype;

          if (read(fd, &ltype, 4) != 4)
            {
              goto out;
            }

          if (ltype == 0x69766f6d)     /* "movi": descend, do not skip */
            {
              continue;
            }

          lseek(fd, size - 4 + (size & 1), SEEK_CUR);
        }
      else if (fcc == 0x63643030)      /* "00dc": one MJPEG frame */
        {
          uint32_t t0 = now_ms();
          int ev;

          if (size > CHUNK_MAX)
            {
              lseek(fd, size + (size & 1), SEEK_CUR);
              continue;
            }

          if (read(fd, chunk, size) != (ssize_t)size)
            {
              goto out;
            }

          if ((size & 1) != 0)
            {
              lseek(fd, 1, SEEK_CUR);
            }

            {
              extern int bk7258_jpegdec_decode(const uint8_t *jpg,
                                               size_t len, uint8_t *out,
                                               int width, int height);

              extern int bk7258_dma2d_split(const void *src, void *dstl,
                                            void *dstr);
              extern int bk7258_gc9d01_blast_pair(const void *right,
                                                  const void *left);

              if (bk7258_jpegdec_decode(chunk, size, g_yuyv,
                                        AVI_W, EYE_YRES) == 0 &&
                  bk7258_dma2d_split(g_yuyv, g_rgbl, g_rgbr) == 0)
                {
                  bk7258_gc9d01_blast_pair(g_rgbr, g_rgbl);
                }
            }

          ev = btn_event(btnfd);
          if (ev != 0)
            {
              ret = ev;
              goto out;
            }

            {
              uint32_t spent = now_ms() - t0;

              if (spent < 33)
                {
                  usleep((33 - spent) * 1000);
                }
            }
        }
      else
        {
          lseek(fd, size + (size & 1), SEEK_CUR);
        }
    }

out:
  free(chunk);
  close(fd);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Controller bring-up runs off the console thread so a hang is a
 * report rather than a dead board.  state: 1 = running, 2 = returned.
 */

static volatile int g_bt_ctrl_state;
static volatile int g_bt_ctrl_ret;

static void *bt_ctrl_thread(void *arg)
{
  extern int bk7258_bt_controller_init(void);

  g_bt_ctrl_ret = bk7258_bt_controller_init();
  g_bt_ctrl_state = 2;
  return NULL;
}

int main(int argc, char *argv[])
{
  int btnfd;
  int cur = -1;                        /* -1 = NEUTRAL idle loop */

  mount("/dev/mmcsd0", "/mnt", "vfat", 0, NULL);

  g_yuyv = malloc(AVI_W * EYE_YRES * 2 + 64);
  g_rgbl = memalign(4, EYE_XRES * EYE_YRES * 2);
  g_rgbr = memalign(4, EYE_XRES * EYE_YRES * 2);
  if (g_yuyv == NULL || g_rgbl == NULL || g_rgbr == NULL)
    {
      return 1;
    }

  btnfd = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
  g_btn_last = 0;

    {
      extern void bk7258_gpio_config(int pin, bool output, bool pullup,
                                     bool pulldown);

      bk7258_gpio_config(12, false, true, false);
      bk7258_gpio_config(8, false, true, false);
      printf("face: btnfd=%d (diagnostic; raw GPIO poll is primary)\n",
             btnfd);
    }

  if (argc > 1 && strcmp(argv[1], "CAL") == 0)
    {
      /* Color calibration: decode NEUTRAL frame 1 (host knows its
       * ground truth: cream background RGB565 ~0xde96), then run the
       * DMA2D with every input-format interpretation and print sample
       * pixels.  The right combo identifies itself.
       */

      extern int bk7258_jpegdec_decode(const uint8_t *jpg, size_t len,
                                       uint8_t *out, int w, int h);
      extern int bk7258_dma2d_split(const void *src, void *dstl,
                                    void *dstr);
      extern void bk7258_dma2d_set_fgcfg(uint32_t fmt2, int reve);

      uint8_t *chunk = malloc(CHUNK_MAX);
      int fd = open("/mnt/NEUTRAL.AVI", O_RDONLY);
      uint32_t hdr[3];
      uint32_t fmt;
      int reve;

      if (fd < 0 || chunk == NULL)
        {
          return 1;
        }

      read(fd, hdr, 12);
      for (; ; )
        {
          if (read(fd, hdr, 8) != 8) return 1;
          if (hdr[0] == 0x5453494c)
            {
              uint32_t lt; read(fd, &lt, 4);
              if (lt == 0x69766f6d) continue;
              lseek(fd, hdr[1] - 4 + (hdr[1] & 1), SEEK_CUR);
            }
          else if (hdr[0] == 0x63643030)
            {
              read(fd, chunk, hdr[1]);
              break;
            }
          else
            {
              lseek(fd, hdr[1] + (hdr[1] & 1), SEEK_CUR);
            }
        }
      close(fd);

      if (bk7258_jpegdec_decode(chunk, hdr[1], g_yuyv,
                                AVI_W, EYE_YRES) != 0)
        {
          printf("cal: decode failed\n");
          return 1;
        }

      printf("cal: expect bg~de96 (cream)\n");
      for (fmt = 0; fmt < 4; fmt++)
        {
          for (reve = 0; reve < 2; reve++)
            {
              uint16_t *o = (uint16_t *)g_rgbl;

              bk7258_dma2d_set_fgcfg(fmt, reve);
              memset(g_rgbl, 0, 64);
              if (bk7258_dma2d_split(g_yuyv, g_rgbl, g_rgbr) == 0)
                {
                  printf("cal: fmt%lu reve%d -> %04x %04x %04x %04x\n",
                         (unsigned long)fmt, reve,
                         o[10 * 160 + 8], o[10 * 160 + 9],
                         o[10 * 160 + 10], o[10 * 160 + 11]);
                }
              else
                {
                  printf("cal: fmt%lu reve%d -> ERR\n",
                         (unsigned long)fmt, reve);
                }
            }
        }

      bk7258_dma2d_set_fgcfg(0, 0);
      free(chunk);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "MIC") == 0)
    {
      /* Live talkback: hardware ADC->DAC loop, zero CPU in the path.
       * face MIC [sec] (default 10).  Howling near the speaker is a
       * healthy sign.
       */

      extern int bk7258_audio_adc_init(bool rate16k);
      extern int bk7258_audio_adc_start(void);
      extern int bk7258_audio_adc_stop(void);
      extern int bk7258_audio_dac_init(bool rate16k);
      extern int bk7258_audio_dac_start(void);
      extern int bk7258_audio_dac_stop(void);
      extern int bk7258_audio_loopback(bool on);
      int sec = (argc > 2) ? atoi(argv[2]) : 10;

      if (sec < 3)
        {
          sec = 3;
        }
      else if (sec > 30)
        {
          sec = 30;
        }

      if (bk7258_audio_adc_init(true) != 0 ||
          bk7258_audio_dac_init(true) != 0)
        {
          printf("face: audio init failed\n");
          return 1;
        }

      bk7258_audio_adc_start();
      bk7258_audio_dac_start();
      bk7258_audio_loopback(true);
      printf("face: live mic for %d s -- talk!\n", sec);
      sleep(sec);
      bk7258_audio_loopback(false);
      bk7258_audio_dac_stop();
      bk7258_audio_adc_stop();
      printf("face: mic done\n");
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "REC") == 0)
    {
      /* Closed-loop mic test: face REC [sec].  The motor buzz is the
       * "start talking" cue, then sec seconds of MIC1 go to a WAV on
       * the card and straight back out the speaker.
       */

      extern int bk7258_audio_adc_init(bool rate16k);
      extern int bk7258_audio_adc_start(void);
      extern int bk7258_audio_adc_stop(void);
      extern int bk7258_audio_adc_read(int16_t *pcm, int nsamples);
      extern int bk7258_audio_adc_read2(uint32_t *pairs, int nsamples);
      extern int bk7258_audio_dac_init(bool rate16k);
      extern int bk7258_audio_dac_start(void);
      extern int bk7258_audio_dac_stop(void);
      extern int bk7258_audio_dac_write(const int16_t *pcm, int nsamples);
      extern int bk7258_motor_init(void);
      extern int bk7258_motor_on(int duty_pct);
      extern int bk7258_motor_off(void);

      int sec = (argc > 2) ? atoi(argv[2]) : 5;
      int total;
      int peakl = 0;
      int peakr = 0;
      uint32_t *pairs;
      int16_t *pcm;
      int16_t junk[160];
      int fd;
      int i;
      int ret;

      if (sec < 1)
        {
          sec = 1;
        }
      else if (sec > 10)
        {
          sec = 10;
        }

      total = sec * 16000;
      pairs = malloc(total * 4);
      pcm = malloc(total * 2);
      if (pcm == NULL || pairs == NULL)
        {
          free(pairs);
          free(pcm);
          return 1;
        }

      ret = bk7258_audio_adc_init(true);
      if (ret != 0)
        {
          printf("face: adc init failed: %d\n", ret);
          free(pcm);
          return 1;
        }

      extern int bk7258_audio_beep(int ms);

      if (bk7258_audio_dac_init(true) != 0)
        {
          printf("face: dac init failed\n");
          free(pairs);
          free(pcm);
          return 1;
        }

      /* The cue is a beep -- the motor buzz went unnoticed in testing */

      bk7258_motor_init();
      bk7258_motor_on(30);
      bk7258_audio_beep(300);
      bk7258_motor_off();
      usleep(300 * 1000);

      printf("face: recording %d s...\n", sec);
      bk7258_audio_adc_start();

      for (i = 0; i < 10; i++)                /* 100 ms settle, discard */
        {
          bk7258_audio_adc_read(junk, 160);
        }

      bk7258_audio_adc_read2(pairs, total);
      bk7258_audio_adc_stop();

      for (i = 0; i < total; i++)
        {
          int l = (int16_t)(pairs[i] & 0xffff);
          int r = (int16_t)(pairs[i] >> 16);

          if (l < 0) l = -l;
          if (r < 0) r = -r;
          if (l > peakl) peakl = l;
          if (r > peakr) peakr = r;
        }

      printf("face: peaks MIC1 %d MIC2 %d -> using %s\n",
             peakl, peakr, peakr > peakl ? "MIC2" : "MIC1");

      for (i = 0; i < total; i++)
        {
          pcm[i] = (peakr > peakl)
                   ? (int16_t)(pairs[i] >> 16)
                   : (int16_t)(pairs[i] & 0xffff);
        }

      free(pairs);
      pairs = NULL;

      fd = open("/mnt/REC.WAV", O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd >= 0)
        {
          uint8_t wav[44] =
          {
            'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
            'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
            0x80, 0x3e, 0, 0, 0, 0x7d, 0, 0, 2, 0, 16, 0,
            'd', 'a', 't', 'a', 0, 0, 0, 0
          };
          uint32_t dl = total * 2;
          uint32_t rl = dl + 36;

          memcpy(wav + 4, &rl, 4);
          memcpy(wav + 40, &dl, 4);
          write(fd, wav, 44);
          write(fd, pcm, dl);
          close(fd);
          printf("face: saved /mnt/REC.WAV\n");
        }

      printf("face: playback...\n");
      ret = bk7258_audio_dac_init(true);
      if (ret == 0)
        {
          bk7258_audio_dac_start();
          bk7258_audio_dac_write(pcm, total);
          bk7258_audio_dac_stop();
        }

      free(pcm);
      printf("face: rec done\n");
      return 0;
    }

  if (argc > 2 && strcmp(argv[1], "PLAY") == 0)
    {
      /* WAV playback through the on-chip DAC: face PLAY <name-on-card>.
       * Mono 8/16 kHz 16-bit PCM only (all vendor prompts qualify).
       */

      extern int bk7258_audio_dac_init(bool rate16k);
      extern int bk7258_audio_dac_start(void);
      extern int bk7258_audio_dac_stop(void);
      extern int bk7258_audio_dac_write(const int16_t *pcm, int nsamples);
      extern int bk7258_audio_dac_write2(const uint32_t *frames,
                                         int nframes);

      char path[48];
      uint8_t hdr[8];
      uint8_t fmt[16];
      uint32_t rate = 0;
      uint32_t dlen = 0;
      uint16_t ch = 0;
      uint16_t bits = 0;
      uint8_t *buf;
      int fd;
      int ret;

      snprintf(path, sizeof(path), "/mnt/%s", argv[2]);
      fd = open(path, O_RDONLY);
      if (fd < 0)
        {
          printf("face: cannot open %s\n", path);
          return 1;
        }

      read(fd, hdr, 8);
      read(fd, hdr, 4);                     /* "WAVE" */
      for (; ; )
        {
          uint32_t csz;

          if (read(fd, hdr, 8) != 8)
            {
              break;
            }

          csz = hdr[4] | (hdr[5] << 8) | ((uint32_t)hdr[6] << 16) |
                ((uint32_t)hdr[7] << 24);

          if (memcmp(hdr, "fmt ", 4) == 0 && csz >= 16)
            {
              read(fd, fmt, 16);
              ch   = fmt[2] | (fmt[3] << 8);
              rate = fmt[4] | (fmt[5] << 8) | ((uint32_t)fmt[6] << 16) |
                     ((uint32_t)fmt[7] << 24);
              bits = fmt[14] | (fmt[15] << 8);
              if (csz > 16)
                {
                  lseek(fd, csz - 16 + (csz & 1), SEEK_CUR);
                }
            }
          else if (memcmp(hdr, "data", 4) == 0)
            {
              dlen = csz;
              break;
            }
          else
            {
              lseek(fd, csz + (csz & 1), SEEK_CUR);
            }
        }

      printf("face: %s %luHz %uch %ubit %lu bytes\n", argv[2],
             (unsigned long)rate, ch, bits, (unsigned long)dlen);

      if (dlen == 0 || ch < 1 || ch > 2 || bits != 16 ||
          (rate != 16000 && rate != 8000))
        {
          printf("face: unsupported format\n");
          close(fd);
          return 1;
        }

      ret = bk7258_audio_dac_init(rate == 16000);
      if (ret != 0)
        {
          printf("face: dac init failed: %d\n", ret);
          close(fd);
          return 1;
        }

      buf = malloc(8192);
      if (buf == NULL)
        {
          close(fd);
          return 1;
        }

      bk7258_audio_dac_start();
      while (dlen > 0)
        {
          int n = read(fd, buf, dlen > 8192 ? 8192 : dlen);

          if (n <= 0)
            {
              break;
            }

          if (ch == 2)
            {
              bk7258_audio_dac_write2((const uint32_t *)buf, n / 4);
            }
          else
            {
              bk7258_audio_dac_write((const int16_t *)buf, n / 2);
            }

          dlen -= n;
        }

      bk7258_audio_dac_stop();
      free(buf);
      close(fd);
      printf("face: play done\n");
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "HCIT") == 0)
    {
      /* What the transport saw.  The driver records the opening frames
       * without touching the console -- printing from the controller's
       * own callback thread was itself enough to change where a host
       * bring-up stalled -- so the dialogue is read back here instead.
       */

      extern void bk7258_hci_trace_dump(void);

      bk7258_hci_trace_dump();
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "PSKEEP") == 0)
    {
      /* Leave controller power save alone for the next bring-up, so the
       * scan that used to work can be measured against the sleeping
       * controller as well as the awake one.
       */

      extern void bk7258_bt_ps_keep(void);

      bk7258_bt_ps_keep();
      printf("face: power save will be left on for the next BT bring-up\n");
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "HCILS") == 0)
    {
      /* Legacy LE scan over raw HCI, as a control for the extended one.
       *
       * Through the Bluetooth service the controller accepts
       * LE_Set_Extended_Scan_Parameters and _Enable with status zero and
       * then reports nothing, exactly as it accepts the extended
       * advertising commands and radiates nothing.  If the same radio
       * hears advertisers when asked with the 4.0 opcodes, the
       * difference is the API generation and not the radio -- which
       * decides whether the problem to solve is in the host's
       * configuration or in the analogue chain.
       */

      static const uint8_t params[] =
      {
        0x01, 0x0b, 0x20, 0x07,        /* LE_Set_Scan_Parameters */
        0x00,                          /* passive */
        0x10, 0x00,                    /* interval 16 x 0.625ms */
        0x10, 0x00,                    /* window   16 x 0.625ms */
        0x00,                          /* own address: public */
        0x00                           /* accept all */
      };

      static const uint8_t enable[] =
      {
        0x01, 0x0c, 0x20, 0x02,        /* LE_Set_Scan_Enable */
        0x01,                          /* enable */
        0x00                           /* no duplicate filtering */
      };

      static const uint8_t disable[] =
      {
        0x01, 0x0c, 0x20, 0x02, 0x00, 0x00
      };

      uint8_t buf[256];
      int fd;
      int waited;
      int reports = 0;
      int other = 0;

      fd = open("/dev/ttyHCI0", O_RDWR | O_NONBLOCK);
      if (fd < 0)
        {
          printf("face: /dev/ttyHCI0 open failed: %d\n", errno);
          return 1;
        }

      while (read(fd, buf, sizeof(buf)) > 0);

      write(fd, params, sizeof(params));
      usleep(300000);
      write(fd, enable, sizeof(enable));

      for (waited = 0; waited < 6000; waited += 50)
        {
          int n = read(fd, buf, sizeof(buf));
          int off = 0;

          while (n > 0 && off + 3 <= n)
            {
              int flen = 3 + buf[off + 2];

              if (off + flen > n)
                {
                  break;
                }

              if (buf[off + 1] == 0x3e)
                {
                  reports++;
                }
              else
                {
                  other++;
                }

              off += flen;
            }

          usleep(50000);
        }

      write(fd, disable, sizeof(disable));
      close(fd);

      printf("face: legacy scan -> %d advertising reports, %d other events\n",
             reports, other);
      printf("face: verdict: %s\n",
             reports > 0 ? "radio hears with the 4.0 opcodes" :
                           "silent with legacy opcodes too");
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "NOPS") == 0)
    {
      /* Take the controller out of power save.
       *
       * enter_normal_app_mode()'s loop reads "if (ble_ps_enabled())
       * rwip_sleep();" before every rwip_schedule(), and ble_ps_enable_set()
       * turns that on unconditionally at start of day -- it ignores its
       * argument and stores 1.  The timing says the same thing from the
       * outside: an LE read answers in 0 ms until the host sends
       * HCI_Reset, and in seconds afterwards, which is what a controller
       * that went idle and now wakes on a timer looks like.
       */

      extern void ble_ps_enable_clear(void);
      extern int ble_ps_enabled(void);

      printf("face: ble power save was %d\n", ble_ps_enabled());
      ble_ps_enable_clear();
      printf("face: ble power save now %d\n", ble_ps_enabled());
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "HCIP") == 0)
    {
      /* Is the transport's poll() wakeup real?
       *
       * Every command the controller answers asynchronously takes
       * exactly 10.000 s to reach the Bluetooth service, while the ones
       * it answers inside write() -- HCI_Reset, and the BR/EDR opcodes
       * it rejects outright -- arrive in 0 ms.  The same command read
       * back through a plain read() loop also arrives immediately.  The
       * service is the only consumer that waits in poll(), so this asks
       * poll() the same question directly: a wakeup that takes seconds
       * when the data was there in milliseconds is a driver bug, not a
       * controller one.
       */

      struct pollfd pfd;
      uint8_t le[4] = { 0x01, 0x03, 0x20, 0x00 };
      uint8_t evt[64];
      struct timespec t0;
      struct timespec t1;
      int fd;
      int ret;
      long ms;

      fd = open("/dev/ttyHCI0", O_RDWR);
      if (fd < 0)
        {
          printf("face: /dev/ttyHCI0 open failed: %d\n", errno);
          return 1;
        }

      write(fd, le, sizeof(le));

      pfd.fd     = fd;
      pfd.events = POLLIN;

      clock_gettime(CLOCK_MONOTONIC, &t0);
      ret = poll(&pfd, 1, 20000);
      clock_gettime(CLOCK_MONOTONIC, &t1);

      ms = (t1.tv_sec - t0.tv_sec) * 1000 +
           (t1.tv_nsec - t0.tv_nsec) / 1000000;

      printf("face: poll -> %d, revents 0x%x, after %ldms\n",
             ret, pfd.revents, ms);

      if (ret > 0)
        {
          int n = read(fd, evt, sizeof(evt));
          printf("face: read %d bytes, evt %02x\n", n, n > 1 ? evt[1] : 0);
        }

      close(fd);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "HCIX") == 0)
    {
      /* The same before/after-reset question as HCIR, decoded instead of
       * weighed.  HCIR counted bytes and called a late HCI_Reset
       * completion an LE answer, which is how it reached the wrong
       * verdict; an opcode is not something you can mistake for another
       * opcode.  Every frame that arrives is printed with the command it
       * completes.
       */

      uint8_t le[4]  = { 0x01, 0x03, 0x20, 0x00 };
      uint8_t rst[4] = { 0x01, 0x03, 0x0c, 0x00 };
      uint8_t buf[192];
      int fd;
      int phase;

      fd = open("/dev/ttyHCI0", O_RDWR | O_NONBLOCK);
      if (fd < 0)
        {
          printf("face: /dev/ttyHCI0 open failed: %d\n", errno);
          return 1;
        }

      while (read(fd, buf, sizeof(buf)) > 0);

      for (phase = 0; phase < 3; phase++)
        {
          static const char *what[3] =
            {
              "LE_Read_Local_Features (before reset)",
              "HCI_Reset",
              "LE_Read_Local_Features (after reset)"
            };

          int budget = (phase == 0) ? 6000 : 12000;
          int waited;
          int seen = 0;

          write(fd, phase == 1 ? rst : le, 4);
          printf("face: --- %s ---\n", what[phase]);

          for (waited = 0; waited < budget; waited += 50)
            {
              int n = read(fd, buf, sizeof(buf));
              int off = 0;

              while (n > 0 && off + 3 <= n)
                {
                  int flen = 3 + buf[off + 2];

                  if (off + flen > n)
                    {
                      break;
                    }

                  if (buf[off + 1] == 0x0e && flen >= 7)
                    {
                      printf("face:   +%dms complete op %02x%02x status %02x\n",
                             waited, buf[off + 5], buf[off + 4],
                             buf[off + 6]);
                    }
                  else
                    {
                      printf("face:   +%dms evt %02x len %d\n",
                             waited, buf[off + 1], flen);
                    }

                  seen++;
                  off += flen;
                }

              usleep(50000);
            }

          if (seen == 0)
            {
              printf("face:   (nothing in %dms)\n", budget);
            }
        }

      close(fd);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "HCIR") == 0)
    {
      /* Does HCI_Reset over the raw path cost the LE side?
       *
       * HCIK ruled out any gating: LE_Read_Local_Supported_Features
       * answers on its own in well under a second when it is the first
       * thing sent.  The Zephyr host sends the same command and never
       * hears back -- and the one thing it does first that HCIK does not
       * is HCI_Reset.  This controller is normally driven by Beken's own
       * host, so a reset that no vendor re-initialisation follows is
       * exactly the kind of thing the raw path would leave half done.
       *
       * Ask the same LE question twice, once either side of a reset.
       */

      uint8_t le[4]  = { 0x01, 0x03, 0x20, 0x00 };   /* LE_Read_Local_Features */
      uint8_t rst[4] = { 0x01, 0x03, 0x0c, 0x00 };   /* HCI_Reset */
      uint8_t evt[96];
      int fd;
      int before;
      int reset;
      int after;
      int waited;

      fd = open("/dev/ttyHCI0", O_RDWR | O_NONBLOCK);
      if (fd < 0)
        {
          printf("face: /dev/ttyHCI0 open failed: %d\n", errno);
          return 1;
        }

      while (read(fd, evt, sizeof(evt)) > 0);

      write(fd, le, sizeof(le));
      before = 0;
      for (waited = 0; waited < 4000; waited += 50)
        {
          int k = read(fd, evt, sizeof(evt));
          if (k > 0)
            {
              before += k;
            }

          usleep(50000);
        }

      write(fd, rst, sizeof(rst));
      reset = 0;
      for (waited = 0; waited < 3000; waited += 50)
        {
          int k = read(fd, evt, sizeof(evt));
          if (k > 0)
            {
              reset += k;
            }

          usleep(50000);
        }

      write(fd, le, sizeof(le));
      after = 0;
      for (waited = 0; waited < 8000; waited += 50)
        {
          int k = read(fd, evt, sizeof(evt));
          if (k > 0)
            {
              after += k;
            }

          usleep(50000);
        }

      printf("face: LE before reset -> %d bytes\n", before);
      printf("face: reset itself    -> %d bytes\n", reset);
      printf("face: LE after reset  -> %d bytes\n", after);
      printf("face: verdict: %s\n",
             (before > 0 && after == 0) ? "HCI_Reset kills the LE side" :
             (before > 0 && after > 0) ? "reset is harmless" :
             "LE did not answer even before the reset");
      close(fd);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "HCIK") == 0)
    {
      /* Does an LE response need a later write to come out?
       *
       * HCIQ showed four LE reads "time out" and then a single 41-byte
       * read carrying all four answers, in order, every status zero --
       * so the controller answers them and something holds the answers
       * back.  If the thing that releases them is the next command
       * written, then a host that sends one command and waits, which is
       * exactly what bt_hci_cmd_send_sync does, waits forever.
       *
       * Send one LE read, listen far longer than any plausible radio
       * latency, then send an unrelated command and listen again.  Two
       * answers arriving after the second write is the whole claim.
       */

      uint8_t le[4] = { 0x01, 0x03, 0x20, 0x00 };   /* LE_Read_Local_Features */
      uint8_t ver[4] = { 0x01, 0x01, 0x10, 0x00 };  /* Read_Local_Version */
      uint8_t evt[96];
      int fd;
      int waited;
      int n;
      int first;

      fd = open("/dev/ttyHCI0", O_RDWR | O_NONBLOCK);
      if (fd < 0)
        {
          printf("face: /dev/ttyHCI0 open failed: %d\n", errno);
          return 1;
        }

      while (read(fd, evt, sizeof(evt)) > 0);       /* start from empty */

      write(fd, le, sizeof(le));
      first = 0;
      for (waited = 0; waited < 8000; waited += 50)
        {
          n = read(fd, evt, sizeof(evt));
          if (n > 0)
            {
              first += n;
            }

          usleep(50000);
        }

      printf("face: after LE write alone, 8s -> %d bytes\n", first);

      write(fd, ver, sizeof(ver));
      n = 0;
      for (waited = 0; waited < 3000; waited += 50)
        {
          int k = read(fd, evt, sizeof(evt));
          if (k > 0)
            {
              n += k;
            }

          usleep(50000);
        }

      printf("face: after a second write, 3s -> %d bytes\n", n);
      printf("face: verdict: %s\n",
             (first == 0 && n > 0) ? "responses need a later write" :
             (first > 0) ? "no gating, LE answered on its own" :
             "no answer either way");
      close(fd);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "HCIQ") == 0)
    {
      /* Which HCI opcodes does this controller actually answer over the
       * raw path?  The Zephyr host stalls on the first LE-group command
       * it sends, with the transport ring empty and nothing dropped, so
       * the question is about the controller and not about us.  These
       * are all zero-parameter reads, safe to issue in any order, and
       * deliberately spread across both groups: 1001/1005 are
       * Informational, the 20xx block is LE.  A missing answer prints as
       * a timeout rather than hanging the shell.
       */

      static const uint16_t probe[] =
      {
        0x1001,   /* Read_Local_Version_Information */
        0x1005,   /* Read_Buffer_Size */
        0x2002,   /* LE_Read_Buffer_Size */
        0x2003,   /* LE_Read_Local_Supported_Features */
        0x2007,   /* LE_Read_Advertising_Channel_Tx_Power */
        0x200f,   /* LE_Read_White_List_Size */
        0x201c,   /* LE_Read_Supported_States */
      };

      uint8_t cmd[4];
      uint8_t evt[64];
      int fd;
      int i;

      fd = open("/dev/ttyHCI0", O_RDWR | O_NONBLOCK);
      if (fd < 0)
        {
          printf("face: /dev/ttyHCI0 open failed: %d\n", errno);
          return 1;
        }

      for (i = 0; i < (int)(sizeof(probe) / sizeof(probe[0])); i++)
        {
          int waited;
          int n = -1;

          cmd[0] = 0x01;
          cmd[1] = (uint8_t)(probe[i] & 0xff);
          cmd[2] = (uint8_t)(probe[i] >> 8);
          cmd[3] = 0x00;

          if (write(fd, cmd, sizeof(cmd)) != (int)sizeof(cmd))
            {
              printf("face: %04x write refused (%d)\n", probe[i], errno);
              continue;
            }

          for (waited = 0; waited < 2000; waited += 20)
            {
              n = read(fd, evt, sizeof(evt));
              if (n > 0)
                {
                  break;
                }

              usleep(20000);
            }

          if (n > 0)
            {
              printf("face: %04x -> evt %02x, %d bytes, status %02x\n",
                     probe[i], evt[1], n, n >= 7 ? evt[6] : 0xff);
            }
          else
            {
              printf("face: %04x -> NO ANSWER\n", probe[i]);
            }
        }

      close(fd);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "HCI") == 0)
    {
      /* End-to-end proof of the /dev/ttyHCI0 transport: an H4-framed
       * HCI_Reset in, a Command Complete out.  Exercises the same path
       * the openvela bluetooth service uses, without any of it.
       * The controller must already be up (face BT 4).
       */

      static const uint8_t reset[4] = { 0x01, 0x03, 0x0c, 0x00 };
      uint8_t evt[32];
      char hex[3 * 16 + 1];
      int fd;
      int n;
      int i;

      fd = open("/dev/ttyHCI0", O_RDWR);
      if (fd < 0)
        {
          printf("face: /dev/ttyHCI0 open failed: %d\n", errno);
          return 1;
        }

      n = write(fd, reset, sizeof(reset));
      printf("face: hci write -> %d\n", n);
      if (n != (int)sizeof(reset))
        {
          close(fd);
          return 1;
        }

      n = read(fd, evt, sizeof(evt));
      close(fd);

      if (n <= 0)
        {
          printf("face: hci read -> %d (no event)\n", n);
          return 1;
        }

      for (i = 0; i < n && i < 16; i++)
        {
          snprintf(hex + i * 3, 4, "%02x ", evt[i]);
        }

      hex[(n < 16 ? n : 16) * 3] = '\0';
      printf("face: hci evt %d bytes [%s]\n", n, hex);

      /* 04 0e 04 xx 03 0c 00 = Command Complete, HCI_Reset, success */

      printf("face: transport %s\n",
             (n >= 7 && evt[0] == 0x04 && evt[1] == 0x0e &&
              evt[4] == 0x03 && evt[5] == 0x0c && evt[6] == 0x00) ?
             "WORKS" : "unexpected frame");
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "BT") == 0)
    {
      /* Staged BLE bring-up: face BT [stage], default 1.
       *   1  register the OSI table   (pure handshake, no hardware)
       *   2  + feature flags          (pure handshake)
       *   3  + PHY and RF tables      (pure handshake; the closed radio
       *                                library reads these the moment
       *                                anything votes for the radio)
       *   4  + start the controller   (powers the radio; first step
       *                                that can genuinely hang)
       *   5  + advertise over raw HCI (transmitter enabled)
       *   6  + scan for other advertisers (radio self-test)
       *   7  + RF calibration (faults: needs a SARADC driver)
       *
       * Stage 3 runs on a thread below this one so that a controller
       * that never returns leaves the console alive to say so, instead
       * of taking the board down with it.
       */

      extern int bk7258_bt_osi_init(void);
      extern int bk7258_bt_feature_init(void);
      extern int bk7258_phy_adapter_init(void);
      extern int bk7258_rf_adapter_init(void);
      extern int bk7258_bt_controller_init(void);

      /* Initialise once per boot.  Re-running the staging over a live
       * stack -- re-registering the OSI table, re-initialising the PHY
       * and RF adapters, re-running calibration -- breaks the radio,
       * and that artefact is what made a second scan always return
       * zero.  Two conclusions drawn from that pattern (advertising
       * wedges the radio; a refused transmit wedges it too) were the
       * harness talking, not the hardware.
       */

      static int s_bt_ready;

      int stage = (argc > 2) ? atoi(argv[2]) : 1;
      int ret;

      if (s_bt_ready)
        {
          goto bt_staged;
        }

      ret = bk7258_bt_osi_init();
      printf("face: bt osi -> %d (%s)\n", ret,
             ret == 0 ? "accepted" : "REJECTED");
      if (ret != 0 || stage < 2)
        {
          return ret == 0 ? 0 : 1;
        }

      ret = bk7258_bt_feature_init();
      printf("face: bt feature -> %d (%s)\n", ret,
             ret == 0 ? "accepted" : "REJECTED");
      if (ret != 0 || stage < 3)
        {
          return ret == 0 ? 0 : 1;
        }

      /* PHY first, then RF: the radio arbiter's error path dereferences
       * the PHY table's logger, so registering it second would fault on
       * the first bad command instead of reporting it.
       */

      ret = bk7258_phy_adapter_init();
      printf("face: phy table -> %d\n", ret);
      ret = bk7258_rf_adapter_init();
      printf("face: rf table -> %d\n", ret);

        {
          /* Hand the synthesiser to bluetooth before the controller
           * brings the transceiver up: it reads the RF config there.
           */

          extern int bk7258_ble_use_bt_pll(void);

          printf("face: bt pll -> %d\n", bk7258_ble_use_bt_pll());
        }

      if (stage == 7)
        {
          extern int bk7258_bt_cal_init(void);

          printf("face: RF calibration starting before controller...\n");
          ret = bk7258_bt_cal_init();
          printf("face: RF calibration -> %d\n", ret);
          if (ret < 0)
            {
              return 1;
            }
        }

      if (stage < 4)
        {
          return 0;
        }

        {
          pthread_t tid;
          pthread_attr_t attr;
          struct sched_param sp;
          int waited;

          g_bt_ctrl_state = 1;
          pthread_attr_init(&attr);
          sp.sched_priority = 90;
          pthread_attr_setschedparam(&attr, &sp);
          pthread_attr_setstacksize(&attr, 8192);

          printf("face: bt controller starting...\n");
          if (pthread_create(&tid, &attr, bt_ctrl_thread, NULL) != 0)
            {
              printf("face: cannot spawn controller thread\n");
              return 1;
            }

          for (waited = 0; waited < 100 && g_bt_ctrl_state == 1; waited++)
            {
              usleep(100 * 1000);
            }

          if (g_bt_ctrl_state == 1)
            {
              printf("face: bt controller STILL RUNNING after 10 s -- "
                     "hung inside the closed init\n");
              return 1;
            }

          printf("face: bt controller -> %d (%s)\n", g_bt_ctrl_ret,
                 g_bt_ctrl_ret == 0 ? "UP" : "failed");
          s_bt_ready = (g_bt_ctrl_ret == 0);

bt_staged:
          if (g_bt_ctrl_ret != 0 || stage < 5)
            {
              return g_bt_ctrl_ret == 0 ? 0 : 1;
            }

          if (stage >= 15)
            {
              extern void bk7258_bt_rf_diag(void);

              bk7258_bt_rf_diag();
              return 0;
            }

          if (stage >= 14)
            {
              extern int bk7258_ble_tx_test(int channel, int seconds);

              printf("face: tx test -> %d\n", bk7258_ble_tx_test(19, 3));
              return 0;
            }

          if (stage >= 13)
            {
              extern int bk7258_ble_hci_reset(void);

              printf("face: hci reset -> %d\n", bk7258_ble_hci_reset());
              return 0;
            }

          if (stage >= 12)
            {
              extern int bk7258_ble_adv_stop(void);

              printf("face: adv stop -> %d\n", bk7258_ble_adv_stop());
              return 0;
            }

          if (stage >= 7)
            {
              printf("face: RF calibration completed before controller\n");
              return 0;
            }

          if (stage >= 6)
            {
              extern int bk7258_ble_scan(int seconds);
              int n = bk7258_ble_scan(10);

              printf("face: scan heard %d advertisers (%s)\n", n,
                     n > 0 ? "RADIO WORKS" : "nothing -- radio suspect");
              return n > 0 ? 0 : 1;
            }

            {
              extern int bk7258_ble_txpwr(int idx);
              extern int bk7258_ble_adv_start(const char *name);

              /* face BT 5 [idx]: report the power index, and override
               * it when one is given, before going on the air.
               */

              extern void bk7258_bt_rf_diag(void);

              /* argv[2] is the stage, so the optional power override
               * is argv[3] -- reading the stage as an index is how an
               * earlier run silently forced the transmitter to 5.
               */

              bk7258_bt_rf_diag();
              bk7258_ble_txpwr(argc > 3 ? atoi(argv[3]) : -1);
              int aret = bk7258_ble_adv_start("EPG-252-2026");

              printf("face: advertising -> %d (%s)\n", aret,
                     aret == 0 ? "ON AIR as EPG-252-2026" : "failed");
              return aret == 0 ? 0 : 1;
            }
        }
    }

  if (argc > 1 && strcmp(argv[1], "VIBE") == 0)
    {
      /* Vibration motor test: face VIBE [ms] [duty%].  Defaults to the
       * vendor operating point, 300 ms at 30%.
       */

      extern int bk7258_motor_init(void);
      extern int bk7258_motor_on(int duty_pct);
      extern int bk7258_motor_off(void);
      int ms = (argc > 2) ? atoi(argv[2]) : 300;
      int duty = (argc > 3) ? atoi(argv[3]) : 30;

      bk7258_motor_init();
      printf("face: vibe %d ms @ %d%%\n", ms, duty);
      bk7258_motor_on(duty);
      usleep(ms * 1000);
      bk7258_motor_off();
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "KEYS") == 0)
    {
      /* Raw button probe: watch the three candidate pins directly and
       * print every level transition, bypassing the buttons driver.
       */

      extern void bk7258_gpio_config(int pin, bool output, bool pullup,
                                     bool pulldown);
      extern bool bk7258_gpio_read(int pin);
      static const int pins[3] = { 8, 12, 13 };
      bool last[3];
      int i;
      int t;

      for (i = 0; i < 3; i++)
        {
          bk7258_gpio_config(pins[i], false, true, false);
          last[i] = bk7258_gpio_read(pins[i]);
          printf("keys: gpio%d idle=%d\n", pins[i], last[i]);
        }

      printf("keys: press each button now (40 s window)\n");
      for (t = 0; t < 8000; t++)
        {
          for (i = 0; i < 3; i++)
            {
              bool v = bk7258_gpio_read(pins[i]);

              if (v != last[i])
                {
                  printf("keys: gpio%d -> %d\n", pins[i], v);
                  last[i] = v;
                }
            }

          usleep(5000);
        }

      printf("keys: done\n");
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "WIRE") == 0)
    {
      /* Wire-order discriminator.  Same three-band pattern (top red,
       * middle green, bottom blue as raw little-endian RGB565) pushed
       * through both display paths at once: devno 0 via the putarea
       * reference route (known correct) and devno 1 via the raw blast
       * route the video player uses.  Whatever the blast panel shows
       * for known inputs uniquely identifies the wire transform.
       */

      extern int bk7258_gc9d01_putfull(int devno, const void *rgb565);
      extern void bk7258_qspi_blast_cpu(int devno, const uint16_t *px,
                                        size_t npx);
      uint16_t *bands = malloc(EYE_XRES * EYE_YRES * 2);
      int i;

      if (bands == NULL)
        {
          return 1;
        }

      for (i = 0; i < EYE_XRES * EYE_YRES; i++)
        {
          int row = i / EYE_XRES;
          bands[i] = row < 53 ? 0xf800 : row < 106 ? 0x07e0 : 0x001f;
        }

      bk7258_gc9d01_putfull(0, bands);
      bk7258_qspi_blast_cpu(1, bands, EYE_XRES * EYE_YRES);
      printf("face: WIRE up. ref panel = red/green/blue; report the "
             "other panel's three band colors top-to-bottom\n");
      free(bands);
      return 0;
    }

  if (argc > 3 && strcmp(argv[1], "RECV") == 0)
    {
      /* Raw file upload over the console: exactly <size> bytes follow,
       * acknowledged with 'K' every 4 KB block so the host self-paces
       * and the tiny UART RX buffer can never overflow.
       */

      const char *path = argv[2];
      long remaining = atol(argv[3]);
      uint8_t *buf = malloc(4096);
      int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

      if (fd < 0 || buf == NULL)
        {
          printf("face: recv setup failed\n");
          return 1;
        }

      printf("face: recv %ld bytes -> %s\nGO\n", remaining, path);
      fflush(stdout);

      while (remaining > 0)
        {
          size_t want = (remaining > 4096) ? 4096 : (size_t)remaining;
          size_t got = 0;

          while (got < want)
            {
              ssize_t n = read(0, buf + got, want - got);

              if (n > 0)
                {
                  got += (size_t)n;
                }
            }

          write(fd, buf, want);
          remaining -= want;
          printf("K");
          fflush(stdout);
        }

      close(fd);
      free(buf);
      printf("\nface: recv done\n");
      return 0;
    }

  if (argc > 1)
    {
      if (play_avi(argv[1], btnfd) < 0)
        {
          printf("face: cannot play %s\n", argv[1]);
        }

      return 0;
    }

  printf("face: NEUTRAL idle; S2 = next expression, S3 = quit\n");

  for (; ; )
    {
      /* Idle = the vendor's genie eye (8.3 mangled name on the card). */

      const char *name = (cur < 0) ? "GENIE_~1" : g_exprs[cur];
      int r = play_avi(name, btnfd);

      if (r == PLAY_QUIT)
        {
          printf("face: quit (>> held)\n");
          break;
        }
      else if (r == PLAY_NEXT)
        {
          cur = (cur < 0) ? 0 : (int)((cur + 1) % NEXPRS);
          printf("face: %s\n", g_exprs[cur]);
        }
      else if (r == PLAY_END)
        {
          if (cur >= 0)
            {
              cur = -1;                /* expression over: back to idle */
            }
        }
      else
        {
          if (cur < 0)
            {
              printf("face: cannot play idle, quitting\n");
              break;
            }

          printf("face: cannot play %s, back to idle\n", name);
          cur = -1;
        }
    }

  if (btnfd >= 0)
    {
      close(btnfd);
    }

  return 0;
}
