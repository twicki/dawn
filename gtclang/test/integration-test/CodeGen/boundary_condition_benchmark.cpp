//===--------------------------------------------------------------------------------*- C++ -*-===//
//                         _       _
//                        | |     | |
//                    __ _| |_ ___| | __ _ _ __   __ _
//                   / _` | __/ __| |/ _` | '_ \ / _` |
//                  | (_| | || (__| | (_| | | | | (_| |
//                   \__, |\__\___|_|\__,_|_| |_|\__, | - GridTools Clang DSL
//                    __/ |                       __/ |
//                   |___/                       |___/
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//
#define DAWN_GENERATED 1
#define GRIDTOOLS_DAWN_HALO_EXTENT 3
#define GT_VECTOR_LIMIT_SIZE 30

#undef FUSION_MAX_VECTOR_SIZE
#undef FUSION_MAX_MAP_SIZE
#define FUSION_MAX_VECTOR_SIZE GT_VECTOR_LIMIT_SIZE
#define FUSION_MAX_MAP_SIZE FUSION_MAX_VECTOR_SIZE
#define BOOST_MPL_LIMIT_VECTOR_SIZE FUSION_MAX_VECTOR_SIZE
#define BOOST_MPL_CFG_NO_PREPROCESSED_HEADERS

#include <gtest/gtest.h>
#include "driver-includes/verify.hpp"
#include "test/integration-test/CodeGen/Macros.hpp"
#include "test/integration-test/CodeGen/Options.hpp"
#include "test/integration-test/CodeGen/generated/boundary_condition_c++-naive.cpp"

#ifndef OPTBACKEND
#define OPTBACKEND gt
#endif

// clang-format off
#include INCLUDE_FILE(test/integration-test/CodeGen/generated/boundary_condition_,OPTBACKEND.cpp)
// clang-format on

using namespace dawn;
TEST(DISABLED_split_stencil, test) {
  domain dom(Options::getInstance().m_size[0], Options::getInstance().m_size[1],
             Options::getInstance().m_size[2]);
  dom.set_halos(GRIDTOOLS_DAWN_HALO_EXTENT, GRIDTOOLS_DAWN_HALO_EXTENT,
                GRIDTOOLS_DAWN_HALO_EXTENT, GRIDTOOLS_DAWN_HALO_EXTENT, 0, 0);
  verifier verif(dom);

  meta_data_t meta_data(dom.isize(), dom.jsize(), dom.ksize() + 1);
  storage_t in_naive(meta_data, "in-naive"), in_gt(meta_data, "in-gt"), out_gt(meta_data, "out-gt"),
      out_naive(meta_data, "out-naive");

  verif.fillMath(8.0, 2.0, 1.5, 1.5, 2.0, 4.0, in_naive, in_gt);
  verif.fill_boundaries(10, in_naive);
  verif.fill(-1.0, out_gt, out_naive);

  dawn_generated::OPTBACKEND::split_stencil copy_gt(dom);
  dawn_generated::cxxnaive::split_stencil copy_naive(dom);

  copy_gt.run(in_gt, out_gt);
  copy_naive.run(in_naive, out_naive);

  ASSERT_TRUE(verif.verify(out_gt, out_naive));
  ASSERT_TRUE(verif.verify(in_gt, in_naive));
}
