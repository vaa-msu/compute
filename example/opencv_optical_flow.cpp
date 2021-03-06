//---------------------------------------------------------------------------//
// Copyright (c) 2013-2014 Mageswaran.D <mageswaran1989@gmail.com>
//
// Distributed under the Boost Software License, Version 1.0
// See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt
//
// See http://kylelutz.github.com/compute for more information.
//---------------------------------------------------------------------------//

#include <iostream>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <boost/compute/source.hpp>
#include <boost/compute/system.hpp>
#include <boost/compute/interop/opencv/core.hpp>
#include <boost/compute/interop/opencv/highgui.hpp>

namespace compute = boost::compute;

// This example shows how to read two images with OpenCV, transfer the
// image to the GPU, and apply a naive optical flow algorithm
// written in OpenCL
int main(int argc, char *argv[])
{
    // Check command line
    if(argc < 3){
        std::cerr << "Usage: " << argv[0]
                  << " ImagePath1 ImagePath2" << std::endl;
        return -1;
    }

    // Read image with OpenCV
    cv::Mat previous_cv_image = cv::imread(argv[1], CV_LOAD_IMAGE_COLOR);
    if(!previous_cv_image.data){
        std::cerr << "Failed to load image" << std::endl;
        return -1;
    }

    // Read image with opencv
    cv::Mat current_cv_image = cv::imread(argv[2], CV_LOAD_IMAGE_COLOR);
    if(!current_cv_image.data){
        std::cerr << "Failed to load image" << std::endl;
        return -1;
    }

    // Get default device and setup context
    compute::device gpu = compute::system::default_device();
    compute::context context(gpu);
    compute::command_queue queue(context, gpu);

    // Convert image to BGRA (OpenCL requires 16-byte aligned data)
    cv::cvtColor(previous_cv_image, previous_cv_image, CV_BGR2BGRA);
    cv::cvtColor(current_cv_image, current_cv_image, CV_BGR2BGRA);

    // Transfer image to gpu
    compute::image2d dev_previous_image =
            compute::opencv_create_image2d_with_mat(
                previous_cv_image, compute::image2d::read_write, queue
                );
    // Transfer image to gpu
    compute::image2d dev_current_image =
            compute::opencv_create_image2d_with_mat(
                current_cv_image, compute::image2d::read_write, queue
                );

    // Create output image
    compute::image2d dev_output_image(
                context,
                compute::image2d::write_only,
                dev_previous_image.get_format(),
                dev_previous_image.width(),
                dev_previous_image.height()
                );

    // Create naive optical flow program
    const char source[] =
    BOOST_COMPUTE_STRINGIZE_SOURCE
            (
                __kernel void optical_flow (
                    read_only
                    image2d_t current_image,
                    read_only image2d_t previous_image,
                    write_only image2d_t optical_flow,
                    const float scale,
                    const float offset,
                    const float lambda,
                    const float threshold )
                {
                    sampler_t sampler = CLK_ADDRESS_CLAMP_TO_EDGE;
                    int2 coords = (int2)(get_global_id(0), get_global_id(1));
                    float4 current_pixel    = read_imagef(current_image,
                                                          sampler,
                                                          coords);
                    float4 previous_pixel   = read_imagef(previous_image,
                                                          sampler,
                                                          coords);
                    int2 x1 	= (int2)(offset, 0.f);
                    int2 y1 	= (int2)(0.f, offset);
                    //get the difference
                    float4 curdif = previous_pixel - current_pixel;

                    //calculate the gradient
                    //Image 2 first
                    float4 gradx = read_imagef(previous_image,
                                               sampler,
                                               coords+x1) -
                                   read_imagef(previous_image,
                                               sampler,
                                               coords-x1);
                    //Image 1
                    gradx += read_imagef(current_image,
                                         sampler,
                                         coords+x1) -
                             read_imagef(current_image,
                                         sampler,
                                         coords-x1);
                    //Image 2 first
                    float4 grady = read_imagef(previous_image,
                                               sampler,
                                               coords+y1) -
                                   read_imagef(previous_image,
                                               sampler,
                                               coords-y1);
                    //Image 1
                    grady += read_imagef(current_image,
                                         sampler,
                                         coords+y1) -
                             read_imagef(current_image,
                                         sampler,
                                         coords-y1);

                    float4 sqr = (gradx*gradx) +
                                 (grady*grady) +
                                 (float4)(lambda,lambda, lambda, lambda);
                    float4 gradmag = sqrt(sqr);

                    ///////////////////////////////////////////////////
                    float4 vx = curdif * (gradx / gradmag);
                    float vxd = vx.x;//assumes greyscale
                    //format output for flowrepos, out(-x,+x,-y,+y)
                    float2 xout = (float2)(fmax(vxd,0.f),fabs(fmin(vxd,0.f)));
                    xout *= scale;
                    ///////////////////////////////////////////////////
                    float4 vy = curdif*(grady/gradmag);
                    float vyd = vy.x;//assumes greyscale
                    //format output for flowrepos, out(-x,+x,-y,+y)
                    float2 yout = (float2)(fmax(vyd,0.f),fabs(fmin(vyd,0.f)));
                    yout *= scale;
                    ///////////////////////////////////////////////////
                    float4 out = (float4)(xout, yout);
                    float cond = (float)isgreaterequal(length(out), threshold);
                    out *= cond;

                    write_imagef(optical_flow, coords, out);
                }
                );

    compute::program optical_program =
            compute::program::create_with_source(source, context);
    optical_program.build();

    // create flip kernel and set arguments
    compute::kernel optical_kernel(optical_program, "optical_flow");
    float scale = 10;
    float offset = 1;
    float lambda = 0.0025;
    float threshold = 1.0;

    optical_kernel.set_arg(0, dev_previous_image);
    optical_kernel.set_arg(1, dev_current_image);
    optical_kernel.set_arg(2, dev_output_image);
    optical_kernel.set_arg(3, scale);
    optical_kernel.set_arg(4, offset);
    optical_kernel.set_arg(5, lambda);
    optical_kernel.set_arg(6, threshold);

    // run flip kernel
    size_t origin[2] = { 0, 0 };
    size_t region[2] = { dev_previous_image.width(),
                         dev_previous_image.height() };
    queue.enqueue_nd_range_kernel(optical_kernel, 2, origin, region, 0);

    // show host image
    cv::imshow("Previous Frame", previous_cv_image);
    cv::imshow("Current Frame", current_cv_image);

    // show gpu image
    compute::opencv_imshow("filtered image", dev_output_image, queue);

    // wait and return
    cv::waitKey(0);
    return 0;
}

