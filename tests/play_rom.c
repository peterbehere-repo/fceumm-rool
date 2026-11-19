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

static size_t audio_batch_cb(const int16_t *d, size_t frames)
{
   samples += frames;
   if (do_hash)
      hash_into(&audio_hash, (const unsigned char *)d, frames * 2 * sizeof(*d));
   return frames;
}

static void input_poll_cb(void) { }

static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
   (void)port; (void)dev; (void)idx; (void)id;
   return 0;
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN)
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
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
            "usage: %s core rom [--frames N] [--sysdir DIR] [--time] [key=value ...]\n",
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

   t0 = clock();
   for (frames = 0; frames < want; frames++)
      c.run();
   t0 = clock() - t0;

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
