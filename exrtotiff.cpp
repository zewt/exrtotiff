// This file is in the public domain.
#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <ImfChannelList.h>
#include "tiffio.h"
#include <stdexcept>
#include <vector>
using namespace std;
using namespace Imf;
using namespace Imath;

void convert(string input_filename, string output_filename)
{
    // This function exits abruptly if it can't open the file.  This isn't a very good library.
    InputFile file(input_filename.c_str());

    Box2i dw = file.header().dataWindow();
    int width  = dw.max.x - dw.min.x + 1;
    int height = dw.max.y - dw.min.y + 1;

    // Request all of the channels from the EXR in the correct order.  We always request
    // in FLOAT, which will convert 16-bit floats to 32-bit for us, since 16-bit floats
    // are rarely supported.  This will also convert 32-bit ints, which isn't ideal,
    // but that's less commonly used.
    //
    // It would be easy to request multiple alpha channels and output them to more EXTRASAMPLES,
    // but without use cases we won't know what to do with them, so for now just handle regular
    // alpha.
    FrameBuffer frameBuffer;
    map<string, vector<float> > channel_data;
    for(auto it = file.header().channels().begin(); it != file.header().channels().end(); ++it)
    {
        string channel_name = it.name();

        vector<float> &buf = channel_data[channel_name];
        buf.resize(height*width, 1);

        frameBuffer.insert(channel_name.c_str(), Slice(FLOAT, (char *) &buf[0], 4 /* bytes */, sizeof(4) * width, 1, 1, 0.0));
    }

    // Map from input channels to output channels.  For example, NX/NY/NZ in a normal
    // map image is mapped to RGB.
    map<string,string> channel_map = {
        { "Z", "Y" },
        { "Y", "Y" },
        { "R", "R" },
        { "G", "G" },
        { "B", "B" },
        { "NX", "R" },
        { "NY", "G" },
        { "NZ", "B" },
        { "A", "A" },
    };

    bool convert_normals = false;
    
    // Make a map from output channels to input channels.
    map<string, string> channel_names;
    for(auto it = file.header().channels().begin(); it != file.header().channels().end(); ++it)
    {
        // The channel name looks like "ABC:def.NX".  Pull out the value after the period,
        // or the whole string if there's no period.
        string channel_name = it.name();
        size_t idx = channel_name.find_last_of('.');
        if(idx != channel_name.npos)
            channel_name = channel_name.substr(idx+1);

        // If this is a normals channel, set convert_normals.
        if(channel_name == "NX")
            convert_normals = true;

        if(channel_map.find(channel_name) == channel_map.end())
        {
            fprintf(stderr, "Unknown channel: %s\n", channel_name.c_str());
            continue;
        }

        string new_name = channel_map[channel_name];

        if(channel_names.find(new_name) != channel_names.end())
        {
            fprintf(stderr, "More than one channel was found that maps to the output channel %s.\n", new_name.c_str());
            exit(1);
        }

        // As a special case, convert "Y" (monochrome) to R, G, B output channels.  Maya doesn't
        // seem to support 32-bit monochrome TIFFs.
        if(new_name == "Y")
        {
            channel_names["R"] = it.name();
            channel_names["G"] = it.name();
            channel_names["B"] = it.name();
        }
        else
        {
            // Store the channel that we'll get this output channel from.  Use the whole layer name,
            // not just the data type portion that we parsed out.
            channel_names[new_name] = it.name();
        }
    }

    vector<vector<float> *> output_channels;
    for(string channel_name: {"R", "G", "B", "A"})
    {
        if(channel_names.find(channel_name) == channel_names.end())
            continue;

        string input_channel_name = channel_names.at(channel_name);
        output_channels.push_back(&channel_data.at(input_channel_name));
    }

    // Read the data for each channel.
    file.setFrameBuffer(frameBuffer);
    file.readPixels(dw.min.y, dw.max.y);

    // On error, TIFFOpen prints an error.
    TIFF *tif = TIFFOpen(output_filename.c_str(), "w");
    if(tif == NULL)
        throw runtime_error("Error opening output file.");

    int channels = output_channels.size();
    bool has_alpha = channel_names.find("A") != channel_names.end();
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, channels);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1);

    // We have RGB data if we have three color channels.
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, (channels - has_alpha) == 3? PHOTOMETRIC_RGB:PHOTOMETRIC_MINISBLACK);
    if(has_alpha)
    {
        uint16 data[] = {
            EXTRASAMPLE_ASSOCALPHA
        };

        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, data);
    }

    // Maya doesn't support COMPRESSION_DEFLATE.
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);

    // Interleave the channels and output the data.
    vector<float> row(width*output_channels.size(), 1);
    for (int y = 0; y < height; y++)
    {
        for(int x = 0; x < width; ++x)
        {
            for(int c = 0; c < (int) output_channels.size(); ++c)
            {
                float value = (*output_channels[c])[y*width + x];

                // Normals in OpenEXR are [-1,+1] floating-point values.  However, even when the data
                // is floating-point, Maya still expects [0,1] data for other file formats.
                if(convert_normals)
                    value = (value / 2) + 0.5;
                row[x*channels+c] = value;
            }
        }
        if(TIFFWriteScanline(tif, &row[0], y, 0) < 0)
            break;
    }

    TIFFClose(tif);
}

int main(int argc, char *argv[])
{
    if(argc != 3)
    {
        printf("Usage: %s input.exr output.exr\n", argv[0]);
        return 1;
    }

    string input_filename = argv[1];
    string output_filename = argv[2];
    try {
        convert(input_filename, output_filename);
    } catch(exception &e) {
        fprintf(stderr, "%s\n", e.what());
        return 0;
    }
    return 0;
}

