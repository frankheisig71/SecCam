#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
//sudo apt update && sudo apt install libstb-dev
#include <stb/stb_image.h>
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

namespace {

struct ImageData {
  int width = 0;
  int height = 0;
  int channels = 1;
  std::vector<uint8_t> pixels;
};

void skip_comments_and_whitespace(std::istream& is) {
  while (is) {
    while (std::isspace(is.peek())) {
      is.get();
    }
    if (is.peek() == '#') {
      is.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    } else {
      break;
    }
  }
}

ImageData read_pnm_image(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open image file: " + path);
  }

  std::string magic;
  input >> magic;
  if (magic != "P5" && magic != "P6") {
    throw std::runtime_error("Unsupported PNM format: " + path);
  }

  int width = 0;
  int height = 0;
  int max_value = 0;

  skip_comments_and_whitespace(input);
  input >> width;
  skip_comments_and_whitespace(input);
  input >> height;
  skip_comments_and_whitespace(input);
  input >> max_value;
  input.get();

  if (!input || width <= 0 || height <= 0 || max_value <= 0 || max_value > 255) {
    throw std::runtime_error("Invalid PNM header in: " + path);
  }

  const int channels = (magic == "P5") ? 1 : 3;
  const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
  std::vector<uint8_t> pixels(pixel_count);
  input.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
  if (input.gcount() != static_cast<std::streamsize>(pixels.size())) {
    throw std::runtime_error("Incomplete PNM pixel data in: " + path);
  }

  return {width, height, channels, std::move(pixels)};
}

ImageData read_image(const std::string& path) {
  // Prüfen, ob es eine PGM- oder PPM-Datei ist
  if (path.size() >= 4) {
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".pgm" || ext == ".ppm") {
      return read_pnm_image(path);
    }
  }

  // Alle anderen Formate (JPEG, PNG, BMP...) mit stb_image als 3-Kanal-RGB laden
  int width = 0;
  int height = 0;
  int orig_channels = 0;
  const int desired_channels = 3;  // Erzwingt RGB (auch bei Graustufen-JPEGs oder RGBA-PNGs)

  uint8_t* raw_data = stbi_load(path.c_str(), &width, &height, &orig_channels, desired_channels);
  if (!raw_data) {
    throw std::runtime_error("Failed to load image (" + std::string(stbi_failure_reason()) + "): " + path);
  }

  const size_t total_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * desired_channels;
  std::vector<uint8_t> pixels(raw_data, raw_data + total_bytes);
  stbi_image_free(raw_data);

  return {width, height, desired_channels, std::move(pixels)};
}

std::vector<float> crop_and_resize_to_model(const std::vector<uint8_t>& src, int width, int height, int channels) {
  constexpr size_t model_width = EI_CLASSIFIER_INPUT_WIDTH;
  constexpr size_t model_height = EI_CLASSIFIER_INPUT_HEIGHT;

  std::vector<float> sample(model_width * model_height, 0.0f);

  if (src.empty() || width <= 0 || height <= 0) {
    return sample;
  }

  // Center Crop berechnen
  const size_t crop_size = static_cast<size_t>(std::min(width, height));
  const size_t crop_x = (static_cast<size_t>(width) - crop_size) / 2U;
  const size_t crop_y = (static_cast<size_t>(height) - crop_size) / 2U;

  for (size_t y = 0; y < model_height; ++y) {
    const size_t src_y = crop_y + (y * crop_size) / model_height;
    for (size_t x = 0; x < model_width; ++x) {
      const size_t src_x = crop_x + (x * crop_size) / model_width;
      const size_t src_index = (src_y * static_cast<size_t>(width) + src_x) * static_cast<size_t>(channels);
      const size_t dst_index = y * model_width + x;

      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;

      if (channels == 1) {
        r = g = b = src[src_index];
      } else {
        r = src[src_index];
        g = src[src_index + 1U];
        b = src[src_index + 2U];
      }

      // 24-Bit RGB Packing für Edge Impulse
      const uint32_t packed_rgb = (static_cast<uint32_t>(r) << 16) |
                                  (static_cast<uint32_t>(g) << 8) |
                                  static_cast<uint32_t>(b);

      sample[dst_index] = static_cast<float>(packed_rgb);
    }
  }

  return sample;
}

void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " <image.jpg|png|pgm|ppm>\n";
  std::cout << "The input image will be center-cropped and resized to "
            << EI_CLASSIFIER_INPUT_WIDTH << "x" << EI_CLASSIFIER_INPUT_HEIGHT << " before inference.\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      print_usage(argv[0]);
      return 1;
    }

    const std::string input_path = argv[1];
    const ImageData image = read_image(input_path);
    const std::vector<float> sample = crop_and_resize_to_model(image.pixels, image.width, image.height, image.channels);

    ei::signal_t signal = {};
    signal.total_length = sample.size();
    signal.get_data = [&sample](size_t offset, size_t length, float* out_ptr) {
      if (out_ptr == nullptr || offset >= sample.size()) {
        return 0;
      }
      const size_t copy_len = std::min(length, sample.size() - offset);
      std::copy_n(sample.begin() + static_cast<std::ptrdiff_t>(offset),
                  static_cast<std::ptrdiff_t>(copy_len),
                  out_ptr);
      return static_cast<int>(copy_len);
    };

    ei_impulse_result_t result = {};
    const int status = run_classifier(&signal, &result, false);
    if (status != EI_IMPULSE_OK) {
      std::cerr << "Inference failed with status=" << status << '\n';
      return 2;
    }

    std::cout << "Model input: " << EI_CLASSIFIER_INPUT_WIDTH << "x" << EI_CLASSIFIER_INPUT_HEIGHT << '\n';
    std::cout << "Labels: " << EI_CLASSIFIER_LABEL_COUNT << '\n';

    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; ++i) {
      const char* label = result.classification[i].label;
      const float value = result.classification[i].value;
      std::cout << "  [" << i << "] " << label << " = " << value << '\n';
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
  }
}
