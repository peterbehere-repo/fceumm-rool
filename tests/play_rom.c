/* Headless playback of a content file through the core: run N frames
 * with no input and report what came out.
 *
 * Prints, at the end of the run, the frame count, a video hash over
 * every visible pixel of every frame with its pitch, how many frames
 * repeated the one before (a duplicated frame the frontend would have
 * to hide), an audio hash, and the samples handed over. Two builds of
 * the core that agree on all of it produced the same thing.
 *
 * --time turns timing on and hashing off: hashing work belongs to a
 * correctness run, not to a run being measured.
 *
 * Linux:
 *   cc -std=c89 -pedantic -Wall -O1 -rdynamic \
 *      -Isrc/drivers/libretro/libretro-common/include \
 *      tests/play_rom.c -o play_rom -ldl
 *   ./play_rom ./fceumm_libretro.so content.nes --frames 600
 *   ./play_rom ./fceumm_libretro.so content.nes --sysdir . fceumm_region=PAL
 *
 * Driving the pad: --input takes a script of frame ranges and buttons,
 * separated by commas or semicolons:
 *
 *   --input "60:start, 120-300:right, 400-420:a+b, 500+:up"
 *
 * A single number is one frame, A-B is a range with both ends included,
 * A+ runs from A to the end of the run. Buttons joined by + are held
 * together: a b select start up down left right l2 (l/r on the core's
 * pad) r2.  Any button not named in a step is released, so steps are
 * absolute rather than cumulative.
 *
 *   --shot FRAME:PATH      write that frame as a binary PPM (P6)
 *   --save FRAME:PATH      write a savestate at that frame
 *   --load PATH            restore a savestate before the first frame
 *   --offset N             add N to the frame number the input script and
 *                          the --shot/--save frames are measured against,
 *                          so a script written for a long run also works
 *                          when resuming from a state taken part-way in
 *   --verbose              show the core's own log output
 *
 * Snapshots are taken after the frame runs, so --shot 300 shows what a
 * frontend would display for frame 300. *
 * A content file that needs a BIOS (an FDS image) reads it from the
 * system directory, which is the current directory unless --sysdir
 * names another. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "libretro.h"

#ifdef _WIN32
#include <windows.h>
#define LIB_OPEN(p)    ((void *)LoadLibraryA(p))
#define LIB_SYM(h, s)  ((void *)GetProcAddress((HMODULE)(h), (s)))
#else
#include <dlfcn.h>
#define LIB_OPEN(p)    dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define LIB_SYM(h, s)  dlsym(h, s)
#endif

#define MAX_OPTIONS 256

static struct retro_variable options[MAX_OPTIONS];
static unsigned num_options;
static const char *system_dir = ".";
static int do_hash = 1;
static int verbose = 0;

/* Scripted pad input. */
#define MAX_STEPS 64
#define MAX_SHOTS 16
struct pad_step { unsigned long from, to; unsigned mask; };
struct shot     { unsigned long frame; const char *path; };
static struct pad_step steps[MAX_STEPS];
static unsigned num_steps;
static struct shot shots[MAX_SHOTS];
static unsigned num_shots;
static const char *save_frame_path;   /* --save */
static unsigned long save_frame = (unsigned long)-1;
static const char *load_state_path;

/* libretro joypad ids, in the order the input script names them. */
static const struct { const char *name; unsigned id; } pad_buttons[] = {
   { "b",      RETRO_DEVICE_ID_JOYPAD_B },
   { "y",      RETRO_DEVICE_ID_JOYPAD_Y },
   { "select", RETRO_DEVICE_ID_JOYPAD_SELECT },
   { "start",  RETRO_DEVICE_ID_JOYPAD_START },
   { "up",     RETRO_DEVICE_ID_JOYPAD_UP },
   { "down",   RETRO_DEVICE_ID_JOYPAD_DOWN },
   { "left",   RETRO_DEVICE_ID_JOYPAD_LEFT },
   { "right",  RETRO_DEVICE_ID_JOYPAD_RIGHT },
   { "a",      RETRO_DEVICE_ID_JOYPAD_A },
   { "x",      RETRO_DEVICE_ID_JOYPAD_X },
   { "l",      RETRO_DEVICE_ID_JOYPAD_L },
   { "r",      RETRO_DEVICE_ID_JOYPAD_R },
   { "l2",     RETRO_DEVICE_ID_JOYPAD_L2 },
   { "r2",     RETRO_DEVICE_ID_JOYPAD_R2 },
   { NULL, 0 }
};
#define NUM_PAD_BUTTONS (sizeof(pad_buttons) / sizeof(pad_buttons[0]) - 1)

static unsigned long current_frame;
static unsigned long frame_offset;

static unsigned long video_hash  = 2166136261UL;
static unsigned long audio_hash  = 2166136261UL;
static unsigned long frame_hash  = 2166136261UL;
static unsigned long prev_frame_hash;
static unsigned frames_drawn, frames_dup, frames_null;
static unsigned long samples;
static unsigned pixel_bytes = 2;
static unsigned video_w, video_h;

static void hash_into(unsigned long *h, const unsigned char *p, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
      *h = ((*h ^ p[i]) * 16777619UL) & 0xFFFFFFFFUL;
}

#define hash_bytes(p, n) hash_into(&frame_hash, (p), (n))

static void fold_hash(void)
{
   video_hash = ((video_hash ^ frame_hash) * 16777619UL) & 0xFFFFFFFFUL;
   if (frame_hash == prev_frame_hash)
      frames_dup++;
   prev_frame_hash = frame_hash;
}

static const void *last_frame;
static size_t last_pitch;

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
   unsigned y;

   if (!data)
   {
      frames_null++;
      return;
   }
   video_w = w;
   video_h = h;
   last_frame = data;
   last_pitch = pitch;
   frames_drawn++;
   if (do_hash)
   {
      frame_hash = 2166136261UL;
      for (y = 0; y < h; y++)
         hash_bytes((const unsigned char *)data + (size_t)y * pitch,
               (size_t)w * pixel_bytes);
      fold_hash();
   }
}

static void audio_cb(int16_t l, int16_t r)
{
   samples++;
   if (do_hash)
   {
      hash_into(&audio_hash, (const unsigned char *)&l, sizeof(l));
      hash_into(&audio_hash, (const unsigned char *)&r, sizeof(r));
   }
}

static FILE *wav_file;
static unsigned long wav_frames;

static size_t audio_batch_cb(const int16_t *d, size_t frames)
{
   if (wav_file)
   {
      fwrite(d, sizeof(*d), (size_t)frames * 2, wav_file);
      wav_frames += frames;
   }
   samples += frames;
   if (do_hash)
      hash_into(&audio_hash, (const unsigned char *)d, frames * 2 * sizeof(*d));
   return frames;
}

/* --wav: dump the core's audio output as 16-bit stereo, so a change can
 * be listened to and not just hashed. The sizes are patched at exit. */
static void put_u32(unsigned long v)
{
   unsigned char b[4];
   b[0] = (unsigned char)(v & 0xFF);
   b[1] = (unsigned char)((v >> 8) & 0xFF);
   b[2] = (unsigned char)((v >> 16) & 0xFF);
   b[3] = (unsigned char)((v >> 24) & 0xFF);
   fwrite(b, 1, 4, wav_file);
}

static void put_u16(unsigned v)
{
   unsigned char b[2];
   b[0] = (unsigned char)(v & 0xFF);
   b[1] = (unsigned char)((v >> 8) & 0xFF);
   fwrite(b, 1, 2, wav_file);
}

/* 16-bit stereo at 48 kHz; FCEUmm's libretro frontend output rate is
 * fixed at 48 kHz with the options this harness sets. */
static void wav_open(const char *path)
{
   wav_file = fopen(path, "wb");
   if (!wav_file)
   {
      fprintf(stderr, "play_rom: cannot write %s\n", path);
      exit(2);
   }
   fwrite("RIFF", 1, 4, wav_file);
   put_u32(0);                       /* patched at close */
   fwrite("WAVE", 1, 4, wav_file);
   fwrite("fmt ", 1, 4, wav_file);
   put_u32(16);
   put_u16(1);                       /* PCM */
   put_u16(2);                       /* channels */
   put_u32(48000);
   put_u32(48000 * 4);               /* byte rate */
   put_u16(4);                       /* block align */
   put_u16(16);                      /* bits */
   fwrite("data", 1, 4, wav_file);
   put_u32(0);                       /* patched at close */
}

static void wav_close(void)
{
   unsigned long data, riff;

   if (!wav_file)
      return;
   data = wav_frames * 4;
   riff = data + 36;
   fseek(wav_file, 4, SEEK_SET);
   put_u32(riff);
   fseek(wav_file, 40, SEEK_SET);
   put_u32(data);
   fclose(wav_file);
   wav_file = NULL;
}

static void input_poll_cb(void) { }

/* The mask in force for the frame being run: the last step whose range
 * covers the current frame, or nothing if no step has started yet. */
static unsigned pad_mask(unsigned long frame)
{
   unsigned i;
   /* steps are stored in the order written; later steps win */
   for (i = num_steps; i > 0; i--)
   {
      const struct pad_step *st = &steps[i - 1];
      if (frame >= st->from && frame <= st->to)
         return st->mask;
   }
   return 0;
}

static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
   unsigned mask;
   (void)idx;
   if (port != 0 || dev != RETRO_DEVICE_JOYPAD)
      return 0;
   mask = pad_mask(current_frame);
   return (mask & (1u << id)) ? 1 : 0;
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < (verbose ? RETRO_LOG_DEBUG : RETRO_LOG_WARN))
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}


/* Write the most recently shown frame as a binary PPM, which is what a
 * frontend would be displaying at that moment. */
static void write_ppm(const char *path, const void *data,
      unsigned w, unsigned h, size_t pitch)
{
   FILE *f;
   unsigned x, y;
   size_t bytes = pixel_bytes;

   if (!data || !w || !h)
   {
      fprintf(stderr, "no frame to write for %s\n", path);
      return;
   }
   f = fopen(path, "wb");
   if (!f)
   {
      fprintf(stderr, "cannot write %s\n", path);
      return;
   }
   fprintf(f, "P6\n%u %u\n255\n", w, h);
   for (y = 0; y < h; y++)
   {
      const unsigned char *row = (const unsigned char *)data + (size_t)y * pitch;
      for (x = 0; x < w; x++)
      {
         const unsigned char *px = row + (size_t)x * bytes;
         unsigned char rgb[3];
         if (bytes == 4)   /* 0RGB1555 in the low two bytes of each word */
         {
            unsigned v = (unsigned)px[0] | ((unsigned)px[1] << 8);
            unsigned r5 = (v >> 10) & 0x1F, g5 = (v >> 5) & 0x1F, b5 = v & 0x1F;
            rgb[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
            rgb[1] = (unsigned char)((g5 << 3) | (g5 >> 2));
            rgb[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
         }
         else              /* RGB565 */
         {
            unsigned v = (unsigned)px[0] | ((unsigned)px[1] << 8);
            unsigned r5 = (v >> 11) & 0x1F, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
            rgb[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
            rgb[1] = (unsigned char)((g6 << 2) | (g6 >> 4));
            rgb[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
         }
         fwrite(rgb, 1, 3, f);
      }
   }
   fclose(f);
   fprintf(stderr, "frame %lu -> %s\n", current_frame, path);
}

/* Parse "A:B" into a frame number and a path. */
static int split_frame_path(const char *arg, unsigned long *frame, const char **path)
{
   char *end;
   unsigned long v = strtoul(arg, &end, 10);
   if (end == arg || *end != ':')
      return 0;
   *frame = v;
   *path  = end + 1;
   return 1;
}

static unsigned parse_buttons(const char *s, const char *whole)
{
   unsigned mask = 0;
   while (*s)
   {
      const char *start = s;
      size_t len;
      unsigned b;
      int found = 0;
      while (*s && *s != '+')
         s++;
      len = (size_t)(s - start);
      for (b = 0; b < NUM_PAD_BUTTONS; b++)
         if (strlen(pad_buttons[b].name) == len
               && !strncmp(pad_buttons[b].name, start, len))
         {
            mask |= 1u << pad_buttons[b].id;
            found = 1;
            break;
         }
      if (!found)
         fprintf(stderr, "unknown button in \"%s\": %.*s\n", whole, (int)len, start);
      if (*s == '+')
         s++;
   }
   return mask;
}

/* "60:start", "120-300:right", "500+:up", steps separated by , or ; */
static void parse_input(const char *script)
{
   const char *p = script;
   while (*p)
   {
      unsigned long from, to;
      char *end;
      const char *seg_end;
      size_t len;
      char buf[128];

      while (*p == ',' || *p == ';' || *p == ' ')
         p++;
      if (!*p)
         break;
      seg_end = p;
      while (*seg_end && *seg_end != ',' && *seg_end != ';')
         seg_end++;
      len = (size_t)(seg_end - p);
      if (len >= sizeof(buf))
         len = sizeof(buf) - 1;
      memcpy(buf, p, len);
      buf[len] = 0;

      from = strtoul(buf, &end, 10);
      if (end == buf)
      {
         fprintf(stderr, "bad input step \"%s\"\n", buf);
         p = seg_end;
         continue;
      }
      if (*end == '-')
      {
         to = strtoul(end + 1, &end, 10);
      }
      else if (*end == '+')
      {
         to = (unsigned long)-1;
         end++;
      }
      else
         to = from;
      if (*end != ':')
      {
         fprintf(stderr, "input step needs buttons: \"%s\"\n", buf);
         p = seg_end;
         continue;
      }
      if (num_steps < MAX_STEPS)
      {
         steps[num_steps].from = from;
         steps[num_steps].to   = to;
         steps[num_steps].mask = parse_buttons(end + 1, buf);
         num_steps++;
      }
      else
         fprintf(stderr, "too many input steps, ignoring \"%s\"\n", buf);
      p = seg_end;
   }
}

static bool env_cb(unsigned cmd, void *data)
{
   unsigned i;

   switch (cmd)
   {
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         pixel_bytes = (*(const enum retro_pixel_format *)data
               == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char **)data = system_dir;
         return true;
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback *)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool *)data = true;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable *)data;
         for (i = 0; i < num_options; i++)
            if (!strcmp(var->key, options[i].key))
            {
               var->value = options[i].value;
               return true;
            }
         return false;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool *)data = false;
         return true;
      default:
         return false;
   }
}

typedef struct
{
   void (*set_environment)(retro_environment_t);
   void (*set_video_refresh)(retro_video_refresh_t);
   void (*set_audio_sample)(retro_audio_sample_t);
   void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*set_input_poll)(retro_input_poll_t);
   void (*set_input_state)(retro_input_state_t);
   void (*init)(void);
   void (*deinit)(void);
   bool (*load_game)(const struct retro_game_info *);
   void (*unload_game)(void);
   void (*run)(void);
   void (*get_system_info)(struct retro_system_info *);
   void (*get_av_info)(struct retro_system_av_info *);
   size_t (*serialize_size)(void);
   bool (*serialize)(void *, size_t);
   bool (*unserialize)(const void *, size_t);
} core_t;

static int load_sym(void *lib, const char *name, void *dst)
{
   void *p = LIB_SYM(lib, name);
   if (!p)
   {
      fprintf(stderr, "missing %s\n", name);
      return 0;
   }
   memcpy(dst, &p, sizeof(p));
   return 1;
}

static unsigned char *read_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   unsigned char *buf;
   long n;

   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   buf = (unsigned char *)malloc((size_t)n ? (size_t)n : 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)
   {
      free(buf);
      fclose(f);
      return NULL;
   }
   fclose(f);
   *size = (size_t)n;
   return buf;
}

int main(int argc, char **argv)
{
   core_t c;
   void *lib;
   struct retro_game_info gi;
   struct retro_system_info si;
   struct retro_system_av_info av;
   unsigned char *data;
   size_t size;
   unsigned long frames = 0, want = 600;
   int i;
   clock_t t0;
   const char *core_path, *rom_path;

   if (argc < 3)
   {
      fprintf(stderr,
            "usage: %s core rom [--frames N] [--sysdir DIR] [--time] [--verbose]\n"
            "       [--input SCRIPT] [--shot FRAME:PATH] [--save FRAME:PATH]\n"
            "       [--load PATH] [--wav PATH] [key=value ...]\n",
            argv[0]);
      return 2;
   }
   core_path = argv[1];
   rom_path  = argv[2];

   for (i = 3; i < argc; i++)
   {
      if (!strcmp(argv[i], "--time"))       { do_hash = 0; continue; }
      if (!strcmp(argv[i], "--frames") && i + 1 < argc) { want = (unsigned long)strtoul(argv[++i], NULL, 10); continue; }
      if (!strcmp(argv[i], "--sysdir") && i + 1 < argc) { system_dir = argv[++i]; continue; }
      if (!strcmp(argv[i], "--verbose"))    { verbose = 1; continue; }
      if (!strcmp(argv[i], "--offset") && i + 1 < argc) { frame_offset = (unsigned long)strtoul(argv[++i], NULL, 10); continue; }
      if (!strcmp(argv[i], "--input") && i + 1 < argc) { parse_input(argv[++i]); continue; }
      if (!strcmp(argv[i], "--load") && i + 1 < argc)  { load_state_path = argv[++i]; continue; }
      if (!strcmp(argv[i], "--wav") && i + 1 < argc)   { wav_open(argv[++i]); continue; }
      if (!strcmp(argv[i], "--shot") && i + 1 < argc)
      {
         unsigned long f;
         const char *path;
         if (!split_frame_path(argv[++i], &f, &path))
         { fprintf(stderr, "--shot needs FRAME:PATH\n"); return 2; }
         if (num_shots < MAX_SHOTS)
         { shots[num_shots].frame = f; shots[num_shots].path = path; num_shots++; }
         continue;
      }
      if (!strcmp(argv[i], "--save") && i + 1 < argc)
      {
         if (!split_frame_path(argv[++i], &save_frame, &save_frame_path))
         { fprintf(stderr, "--save needs FRAME:PATH\n"); return 2; }
         continue;
      }
      {
         char *eq = strchr(argv[i], '=');
         if (!eq || num_options >= MAX_OPTIONS)
         {
            fprintf(stderr, "ignoring %s\n", argv[i]);
            continue;
         }
         *eq = '\0';
         options[num_options].key   = argv[i];
         options[num_options].value = eq + 1;
         num_options++;
      }
   }

   lib = LIB_OPEN(core_path);
   if (!lib)
   {
      fprintf(stderr, "cannot load %s\n", core_path);
      return 1;
   }
   memset(&c, 0, sizeof(c));
   if (!load_sym(lib, "retro_set_environment", &c.set_environment)
         || !load_sym(lib, "retro_set_video_refresh", &c.set_video_refresh)
         || !load_sym(lib, "retro_set_audio_sample", &c.set_audio_sample)
         || !load_sym(lib, "retro_set_audio_sample_batch", &c.set_audio_sample_batch)
         || !load_sym(lib, "retro_set_input_poll", &c.set_input_poll)
         || !load_sym(lib, "retro_set_input_state", &c.set_input_state)
         || !load_sym(lib, "retro_init", &c.init)
         || !load_sym(lib, "retro_deinit", &c.deinit)
         || !load_sym(lib, "retro_load_game", &c.load_game)
         || !load_sym(lib, "retro_unload_game", &c.unload_game)
         || !load_sym(lib, "retro_run", &c.run)
         || !load_sym(lib, "retro_get_system_info", &c.get_system_info)
         || !load_sym(lib, "retro_get_system_av_info", &c.get_av_info))
      return 1;

   /* States are optional: a core that does not offer them can still be
    * played, it just cannot be asked for one. */
   if (!load_sym(lib, "retro_serialize_size", &c.serialize_size)
         || !load_sym(lib, "retro_serialize", &c.serialize)
         || !load_sym(lib, "retro_unserialize", &c.unserialize))
      c.serialize_size = NULL;

   c.set_environment(env_cb);
   c.set_video_refresh(video_cb);
   c.set_audio_sample(audio_cb);
   c.set_audio_sample_batch(audio_batch_cb);
   c.set_input_poll(input_poll_cb);
   c.set_input_state(input_state_cb);
   c.init();

   memset(&si, 0, sizeof(si));
   c.get_system_info(&si);

   /* A core that opens the content itself gets only the path; one that
    * takes the bytes gets them, and the path so it can name the game. */
   data = NULL;
   size = 0;
   if (!si.need_fullpath)
   {
      data = read_file(rom_path, &size);
      if (!data)
      {
         fprintf(stderr, "cannot read %s\n", rom_path);
         return 1;
      }
   }

   memset(&gi, 0, sizeof(gi));
   gi.path = rom_path;
   gi.data = data;
   gi.size = size;
   if (!c.load_game(&gi))
   {
      fprintf(stderr, "core refused %s\n", rom_path);
      return 1;
   }

   memset(&av, 0, sizeof(av));
   c.get_av_info(&av);
   fprintf(stderr, "%.3f fps, %.0f Hz, %ux%u\n",
         (double)av.timing.fps, (double)av.timing.sample_rate,
         av.geometry.base_width, av.geometry.base_height);

   if (load_state_path && c.serialize_size)
   {
      size_t size;
      unsigned char *state = read_file(load_state_path, &size);
      if (!state)
         return 1;
      if (!c.unserialize(state, size))
      {
         fprintf(stderr, "core refused state %s\n", load_state_path);
         free(state);
         return 1;
      }
      free(state);
      fprintf(stderr, "state %s loaded\n", load_state_path);
   }

   t0 = clock();
   for (frames = 0; frames < want; frames++)
   {
      unsigned i;
      current_frame = frames + frame_offset;
      c.run();
      for (i = 0; i < num_shots; i++)
         if (shots[i].frame == current_frame)
            write_ppm(shots[i].path, last_frame, video_w, video_h, last_pitch);
      if (current_frame == save_frame && c.serialize_size && save_frame_path)
      {
         size_t size = c.serialize_size();
         void *state = malloc(size ? size : 1);
         if (state && size && c.serialize(state, size))
         {
            FILE *f = fopen(save_frame_path, "wb");
            if (f)
            {
               fwrite(state, 1, size, f);
               fclose(f);
               fprintf(stderr, "state of frame %lu -> %s (%lu bytes)\n",
                     current_frame, save_frame_path, (unsigned long)size);
            }
            else
               fprintf(stderr, "cannot write %s\n", save_frame_path);
         }
         else
            fprintf(stderr, "core refused to serialize at frame %lu\n", frames);
         free(state);
      }
   }
   t0 = clock() - t0;
   wav_close();

   if (do_hash)
      fprintf(stdout, "frames %lu drawn %u duped %u null %u video %08lx audio %08lx samples %lu\n",
            frames, frames_drawn, frames_dup, frames_null,
            video_hash, audio_hash, samples);
   else
      fprintf(stdout, "frames %lu drawn %u duped %u null %u samples %lu cpu %.3f s %.1f fps\n",
            frames, frames_drawn, frames_dup, frames_null, samples,
            (double)t0 / (double)CLOCKS_PER_SEC,
            (double)frames / ((double)t0 / (double)CLOCKS_PER_SEC));

   c.unload_game();
   c.deinit();
   free(data);
   return 0;
}
