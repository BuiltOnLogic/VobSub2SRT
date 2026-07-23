/*
 *  VobSub2SRT is a simple command line program to convert .idx/.sub subtitles
 *  into .srt text subtitles by using OCR (tesseract). See README.
 *
 *  Copyright (C) 2010-2016 Rüdiger Sonderfeld <ruediger@c-plusplus.de>
 *  Copyright (C) 2026 Christopher Ogloff <chris.ogloff@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mp_msg.h" // MPlayer message framework
#include "vobsub.h" // VobSub reader
#include "spudec.h" // SPU decoder

// Tesseract OCR
#include <tesseract/baseapi.h>

// libpng
#ifdef __has_include /** some systems do not use a subdir for png */
	#if __has_include(<png.h>)
	#include <png.h>
	#elif __has_include(<libpng/png.h>)
	#include <libpng/png.h>
	#endif
#endif

// Builtins/standard libs
#include <memory>
#include <cstdlib>
#include <stdexcept>
#include <cstdint>
#include <climits>
#include <array>
#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include <vector>

// Language and option handling.
#include "langcodes.h++"
#include "cmd_options.h++"
#include "version.h++"


typedef void* vob_t;
typedef void* spu_t;

/** Image inversion code by Ethan Dye [https://github.com/ecdye/VobSub2SRT] */

struct ImageInverter {
  ImageInverter(const unsigned char *image, size_t image_size)
    : inverted_image(new unsigned char[image_size]) {
    for (size_t i = 0; i < image_size; ++i)
      inverted_image[i] = ((255 - image[i]) > 0x80) ? 0xff : 0;
  }
  ~ImageInverter() { delete[] inverted_image; }
  
  unsigned char *inverted_image;
};

// helper struct for caching and fixing end_pts in some cases
struct sub_text_t {
  uint32_t start_pts;
  uint32_t end_pts;
  std::unique_ptr<char[]> text;
  
  sub_text_t(uint32_t start_pts, uint32_t end_pts, std::unique_ptr<char[]> text);
};

sub_text_t::sub_text_t(uint32_t start_pts, uint32_t end_pts, std::unique_ptr<char[]> text)
  : start_pts(start_pts), end_pts(end_pts), text(std::move(text)) {}


/** Converts time stamp in pts format to a string containing the time stamp for the srt format
 *
 * pts (presentation time stamp) is given with a 90kHz resolution (1/90 ms).
 * srt expects a time stamp as  HH:MM:SS,CTS.
 */
std::string pts2srt(unsigned pts) {
  uint32_t ms = pts/90u;
  uint32_t const h = ms / (3600u * 1000u);
  ms -= h * 3600u * 1000u;
  uint32_t const m = ms / (60u * 1000u);
  ms -= m * 60u * 1000u;
  uint32_t const s = ms / 1000u;
  ms %= 1000u;
  
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%02u:%02u:%02u,%03u", h, m, s, ms);
  return buf.data();
}


/** Converts image data to Netpbm PGM format */
void dump_pgm(std::string const &filename, unsigned counter, unsigned width, unsigned height,
              unsigned stride, unsigned char const *image, size_t image_size) {
  
  char buf[500];
  std::snprintf(buf, sizeof(buf), "%s-%04u.pgm", filename.c_str(), counter);
  std::ofstream pgm(buf, std::ios::binary);
  if(pgm) {
    pgm << "P5\n" << width << " " << height << " 255\n";
    for(size_t i = 0; i < image_size; i += stride) {
      pgm.write(reinterpret_cast<const char*>(image + i), width);
    }
  }
}

/** Or, to PNG */
void dump_png(const std::string &filename, unsigned counter, unsigned width, unsigned height,
	      unsigned stride, const unsigned char* image) {
  char buf[500];
  std::snprintf(buf, sizeof(buf), "%s-%04u.png", filename.c_str(), counter);
  FILE* pngfile = std::fopen(buf, "wb");
  if (!pngfile) {
    mp_msg(MSGT_VOBSUB, MSGL_FATAL, "Unable to open PNG file for writing. Ensure you have write permissions.\n");
    return;
  }
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        std::cerr << "Unable to allocate memory for PNG file structure!\n";
        return;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        return;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return;
    }
    png_init_io(png_ptr, pngfile);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    png_write_info(png_ptr, info_ptr);

    for (size_t row = 0; row < height; ++row) {
        png_write_row(png_ptr, const_cast<png_bytep>(image + row * stride));
    }

    png_write_end(png_ptr, nullptr);

    png_destroy_write_struct(&png_ptr, &info_ptr);
    std::fclose(pngfile);
}

int main(int argc, char **argv) {
  bool show = false;
  bool dump_pgmfiles = false;
  bool dump_pngfiles = false;
  int verb = -1;
  bool list_languages = false;
  std::string ifo_file;
  std::string subname;
  std::string lang;
  std::string tess_lang_user;
  std::string blacklist;
  std::string tesseract_user_dir;
  std::string tess_user_dpi = "72";
  int index = -1;
  int y_threshold = 16;
  int min_width = 8;
  int min_height = 1;
  
  {
    cmd_options opts;
    opts.
      add_option("show", show, "Show subtitles being written.").
      add_option("dump-pgm", dump_pgmfiles, "Save subtitles as sequential NetPBM image files (<subname>-<number>.pgm).").
      add_option("dump-png", dump_pngfiles, "Above, but in PNG format.").
      add_option("verbose", verb, "Decoder verbosity, a value of 1 or 2.").
      add_option("ifo", ifo_file, "Name of the IFO file. Default: tries to open <subname>.ifo(case insensitive).\n\t\t\t\tIFO file is optional but may fix empty palette issues!").
      add_option("lang", lang, "Language to select", 'l').
      add_option("langlist", list_languages, "List languages and exit").
      add_option("index", index, "Subtitle index", 'i').
      add_option("tesseract-lang", tess_lang_user, "Desired Tesseract language (e.g. eng, deu, fra, esp)\n\t\t\t\t(Default: autodetect)").
      add_option("tesseract-data", tesseract_user_dir, "Path to Tesseract data (e.g. you have tessdata_best and wish to\n\t\t\t\tuse it. Default: autodetect)").
      add_option("dpi", tess_user_dpi, "Set DPI for Tesseract OCR. Default: 72.").
      add_option("blacklist", blacklist, "Character blacklist to improve the OCR (e.g. \"|\\/`_~<>\")").
      add_option("y-threshold", y_threshold, "Y (luminance) threshold below which colors treated as black (Default: 16)").
      add_option("min-width", min_width, "Minimum width in pixels to consider a subpicture for OCR (Default: 8)").
      add_option("min-height", min_height, "Minimum height in pixels to consider a subpicture for OCR (Default: 1)");
    
    opts.add_unnamed(subname, "subname", "name of the subtitle files without .idx/.sub ending.");
    std::cout << "VobSub2SRT version " << version << '\n';
    if(!opts.parse_cmd(argc, argv)) {
      return 0;
    }
  }

  if(verb>0) {
    verbose = verb; // mplayer verbose level
  }
  mp_msg_init();
  
  // Set Y threshold
  if (y_threshold != 16) {
    std::cout << "Using Y palette threshold: " << y_threshold << "\n";
  }
  
  // Open the sub/idx subtitles
  spu_t spu;
  vob_t vob = vobsub_open(subname.c_str(), ifo_file.empty() ? 0x0 : ifo_file.c_str(), 1, y_threshold, &spu);
  if (!vob || vobsub_get_indexes_count(vob) == 0) {
    std::cerr << "Couldn't open VobSub files '" << subname << ".idx/.sub'\n";
    if (subname.empty() || vob) {
      vobsub_close(vob);
    spudec_free(spu);
    mp_msg_uninit();
    return 1;
    }
  }

  // list languages and exit
  if(list_languages) {
    std::cout << "Languages:\n";
    for(size_t i = 0; i < vobsub_get_indexes_count(vob); ++i) {
      char const *const id = vobsub_get_id(vob, i);
      std::cout << i << ": " << (id ? id : "(no id)") << '\n';
      vobsub_close(vob);
      spudec_free(spu);
      mp_msg_uninit();
    }
    return 0;
  }
  
  // Handle stream Ids and language
  if(!lang.empty() && index >= 0) {
    std::cerr << "Setting both lang and index not supported.\n";
    vobsub_close(vob);
    spudec_free(spu);
    mp_msg_uninit();
    return 1;
  }

  // Default OCR language to eng unless overridden by user or inferred from selected subtitle track.
  char const *tess_lang = tess_lang_user.empty() ? "eng" : tess_lang_user.c_str();
  if(!lang.empty()) {
    if(vobsub_set_from_lang(vob, lang.c_str()) < 0) {
      std::cerr << "No matching language for '" << lang << "' found! (Trying to use default)\n";
    } else if(tess_lang_user.empty()) {
      char const *const lang3 = iso639_1_to_639_3(lang.c_str());
      if(lang3) {
        tess_lang = lang3;
      }
    }
  } else {
    if(index >= 0) {
      if(static_cast<unsigned>(index) >= vobsub_get_indexes_count(vob)) {
        std::cerr << "Index argument out of range: " << index << " ("
             << vobsub_get_indexes_count(vob) << ")\n";
        return 1;
      }
      vobsub_id = index;
    }
    
    if(vobsub_id >= 0) {
      char const *const lang1 = vobsub_get_id(vob, vobsub_id);
      if(lang1 && tess_lang_user.empty()) {
        char const *const lang3 = iso639_1_to_639_3(lang1);
        if(lang3) {
          tess_lang = lang3;
        }
      }
    }
  }

  // Init Tesseract
  tesseract::TessBaseAPI tess_base_api;
  if(tess_base_api.Init(NULL, tess_lang, tesseract::OEM_LSTM_ONLY) == -1) {
    std::cerr << "Failed to initialize Tesseract (OCR).\n";
    return 1;
  }
  
  // Set blacklist if not empty
  if(!blacklist.empty()) {
    tess_base_api.SetVariable("tessedit_char_blacklist", blacklist.c_str());
  }
  
  // Announce tesseract language data dir for verbosity. This needs a bit of work.
  if(verb>=1) {
    std::cout << "Using Tesseract data directory: " << tesseract_user_dir << ".\n";
  }

  // Run Tesseract multithreaded
  std::string tess_parallel = "1";
  tess_base_api.SetVariable("tessedit_parallelize", tess_parallel.c_str());

  // Set DPI to 72, or user-specified with --dpi
  tess_base_api.SetVariable("user_defined_dpi", tess_user_dpi.c_str());

  // Attempt to improve whitespace detection
  std::string tess_rej_spaces = "0";
  std::string tess_improve_thresh = "1";
  tess_base_api.SetVariable("tessedit_use_reject_spaces", tess_rej_spaces.c_str());
  tess_base_api.SetVariable("tosp_improve_thresh", tess_improve_thresh.c_str());
  
  // Open srt output file
  std::string const srt_filename = subname + ".srt";
  FILE *srtout = std::fopen(srt_filename.c_str(), "w");
  if(!srtout) {
    std::cerr << "Could not open .srt file for writing.\n";
    return 1;
  }
  
  // Read subtitles and convert
  /*
  if (subname.empty()) {
      vobsub_close(vob);
      spudec_free(spu);
      tess_base_api.End();
      std::cerr << "Unable to open subtitles. Exiting.\n";
      return 1;
  }*/
  void *packet;
  int timestamp; // pts100
  int len;
  unsigned last_start_pts = 0;
  unsigned sub_counter = 1;
  std::vector<sub_text_t> conv_subs;
  conv_subs.reserve(255);
  
  while ((len = vobsub_get_next_packet(vob, &packet, &timestamp)) > 0) {
    if (timestamp >= 0) {
      //      unsigned char const *scaled_image = nullptr;
      //      size_t scaled_image_size;

      spudec_assemble(spu, reinterpret_cast<unsigned char*>(packet), len, timestamp);
      spudec_heartbeat(spu, timestamp);

      unsigned char const *image;
      unsigned width, height, stride, start_pts, end_pts;
      size_t image_size;
      
      // Get the image data to know dimensions and timing info
      spudec_get_data(spu, &image, &image_size, &width, &height, &stride, &start_pts, &end_pts);
      
      if (start_pts == last_start_pts) {
        continue;
      }

      last_start_pts = start_pts;

	      if(verbose > 0 and static_cast<unsigned>(timestamp) != start_pts) {
		std::cerr << sub_counter << ": time stamp from .idx (" << timestamp
			  << ") doesn't match time stamp from .sub ("
			  << start_pts << ")\n";
	      }

      // While tesseract version 3.05 (and older) handle inverted image (dark                                            
      // background and light text) without problem for 4.x version use dark                                             
      // text on light background.                                                                                       
      // https://tesseract-ocr.github.io/tessdoc/ImproveQuality#image-processing                                          
      
      ImageInverter inverter(image, image_size);
      image = inverter.inverted_image;

      if(dump_pgmfiles) {
	dump_pgm(subname, sub_counter, width, height, stride, image, image_size);
      }
      
      if (dump_pngfiles) {
	dump_png(subname, sub_counter, width, height, stride, image);
      }
    
      tess_base_api.SetPageSegMode(tesseract::PSM_SINGLE_BLOCK);

      // Adjust dimensions for Tesseract based on scaling mode
      int tesseract_width = static_cast<int>(width);
      int tesseract_height = static_cast<int>(height);
      int tesseract_stride = static_cast<int>(stride);
      tess_base_api.SetImage(image, tesseract_width, tesseract_height, 1, tesseract_stride);
      char *tesseract_text = tess_base_api.GetUTF8Text();
      
      std::unique_ptr<char[]> text;
      if (!tesseract_text) {
	text.reset(new char[60]);
	std::strcpy(text.get(), "VobSub2SRT ERROR: OCR failure! Unable to decode subtitle!");
	delete[] tesseract_text;
      } else {
	size_t len = std::strlen(tesseract_text);
	text.reset(new char[len + 1]);
	std::strcpy(text.get(), tesseract_text);
	delete[] tesseract_text;
      }
        
      if (text) {
	if(show) {
	  std::cout << "Line " << sub_counter << ": " << text.get();
	}
	conv_subs.emplace_back(sub_text_t(start_pts, end_pts, std::move(text)));
	sub_counter++;
      }
    }
  }

    // write the file, fixing end_pts when needed
for(unsigned i = 0; i < conv_subs.size(); ++i) {
  if(conv_subs[i].end_pts == UINT_MAX && i+1 < conv_subs.size())
    conv_subs[i].end_pts = conv_subs[i+1].start_pts;
  
  std::fprintf(srtout, "%u\n%s --> %s\n%s\n\n", i+1, pts2srt(conv_subs[i].start_pts).c_str(),
	       pts2srt(conv_subs[i].end_pts).c_str(), conv_subs[i].text.get());
 }

    // Close up shop
tess_base_api.End();
std::fclose(srtout);
std::cout << "Wrote Subtitles to '" << subname << ".srt'\n";
vobsub_close(vob);
spudec_free(spu);
mp_msg_uninit();
return 0;
}
