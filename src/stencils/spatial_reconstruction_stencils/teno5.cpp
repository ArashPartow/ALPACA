/*****************************************************************************************
*                                                                                        *
* This file is part of ALPACA                                                            *
*                                                                                        *
******************************************************************************************
*                                                                                        *
*  \\                                                                                    *
*  l '>                                                                                  *
*  | |                                                                                   *
*  | |                                                                                   *
*  | alpaca~                                                                             *
*  ||    ||                                                                              *
*  ''    ''                                                                              *
*                                                                                        *
* ALPACA is a MPI-parallelized C++ code framework to simulate compressible multiphase    *
* flow physics. It allows for advanced high-resolution sharp-interface modeling          *
* empowered with efficient multiresolution compression. The modular code structure       *
* offers a broad flexibility to select among many most-recent numerical methods covering *
* WENO/T-ENO, Riemann solvers (complete/incomplete), strong-stability preserving Runge-  *
* Kutta time integration schemes, level set methods and many more.                       *
*                                                                                        *
* This code is developed by the 'Nanoshock group' at the Chair of Aerodynamics and       *
* Fluid Mechanics, Technical University of Munich.                                       *
*                                                                                        *
******************************************************************************************
*                                                                                        *
* LICENSE                                                                                *
*                                                                                        *
* ALPACA - Adaptive Level-set PArallel Code Alpaca                                       *
* Copyright (C) 2020 Nikolaus A. Adams and contributors (see AUTHORS list)               *
*                                                                                        *
* This program is free software: you can redistribute it and/or modify it under          *
* the terms of the GNU General Public License as published by the Free Software          *
* Foundation version 3.                                                                  *
*                                                                                        *
* This program is distributed in the hope that it will be useful, but WITHOUT ANY        *
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A        *
* PARTICULAR PURPOSE. See the GNU General Public License for more details.               *
*                                                                                        *
* You should have received a copy of the GNU General Public License along with           *
* this program (gpl-3.0.txt).  If not, see <https://www.gnu.org/licenses/gpl-3.0.html>   *
*                                                                                        *
******************************************************************************************
*                                                                                        *
* THIRD-PARTY tools                                                                      *
*                                                                                        *
* Please note, several third-party tools are used by ALPACA. These tools are not shipped *
* with ALPACA but available as git submodule (directing to their own repositories).      *
* All used third-party tools are released under open-source licences, see their own      *
* license agreement in 3rdParty/ for further details.                                    *
*                                                                                        *
* 1. tiny_xml           : See LICENSE_TINY_XML.txt for more information.                 *
* 2. expression_toolkit : See LICENSE_EXPRESSION_TOOLKIT.txt for more information.       *
* 3. FakeIt             : See LICENSE_FAKEIT.txt for more information                    *
* 4. Catch2             : See LICENSE_CATCH2.txt for more information                    *
*                                                                                        *
******************************************************************************************
*                                                                                        *
* CONTACT                                                                                *
*                                                                                        *
* nanoshock@aer.mw.tum.de                                                                *
*                                                                                        *
******************************************************************************************
*                                                                                        *
* Munich, July 1st, 2020                                                                 *
*                                                                                        *
*****************************************************************************************/
#include "teno5.h"

#include <stdexcept>
#include <cmath>

/**
 * @brief Computes the flux at one cell face according to used TENO-5 scheme. Also See base class.
 * @note The input cell_size is not required in all stencils, but for unified interface all derived classes inherit it.
 * @note Hotpath function.
 */
double TENO5::ApplyImplementation( std::vector<double> const& array, int const stencil_offset, int const stencil_sign, double const ) const {

#ifndef PERFORMANCE
   if(array.size() < stencil_size_) {
      throw std::logic_error("Stencil size for the TENO5 evaluation is longer than the provided array");
   }
#endif

   // Assign values to v_i to enhance readability
   double const v1 = array[downstream_stencil_size_ + stencil_offset - 2 * stencil_sign];
   double const v2 = array[downstream_stencil_size_ + stencil_offset - 1 * stencil_sign];
   double const v3 = array[downstream_stencil_size_ + stencil_offset];
   double const v4 = array[downstream_stencil_size_ + stencil_offset + 1 * stencil_sign];
   double const v5 = array[downstream_stencil_size_ + stencil_offset + 2 * stencil_sign];

   // Compute smoothness indicators si
   double const s11 = coef_smoothness_11_ * v2 + coef_smoothness_12_ * v3 + coef_smoothness_13_ * v4;
   double const s12 = coef_smoothness_14_ * v2 + coef_smoothness_15_ * v4;

   double const s1 = coef_smoothness_1_*s11*s11 + coef_smoothness_2_*s12*s12;

   double const s21 = coef_smoothness_21_ * v3 + coef_smoothness_22_ * v4 + coef_smoothness_23_ * v5;
   double const s22 = coef_smoothness_24_ * v3 + coef_smoothness_25_ * v4 + coef_smoothness_26_ * v5;

   double const s2 = coef_smoothness_1_*s21*s21 + coef_smoothness_2_*s22*s22;

   double const s31 = coef_smoothness_31_ * v1 + coef_smoothness_32_ * v2 + coef_smoothness_33_ * v3;
   double const s32 = coef_smoothness_34_ * v1 + coef_smoothness_35_ * v2 + coef_smoothness_36_ * v3;

   double const s3 = coef_smoothness_1_*s31*s31 + coef_smoothness_2_*s32*s32;

   double const tau5 = std::abs( s3 - s2 );

   double a1 = 1.0 + tau5 / (s1 + epsilon_);
   double a2 = 1.0 + tau5 / (s2 + epsilon_);
   double a3 = 1.0 + tau5 / (s3 + epsilon_);

   // Calculate a^6 without using std::pow to improve performance drastically
   a1 *= (a1*a1);
   a2 *= (a2*a2);
   a3 *= (a3*a3);
   a1 *= a1;
   a2 *= a2;
   a3 *= a3;

   double const one_a_sum_tmp = 1.0 / (a1 + a2 + a3);

   double const b1 = (a1 * one_a_sum_tmp) < CT_ ? 0. : 1.;
   double const b2 = (a2 * one_a_sum_tmp) < CT_ ? 0. : 1.;
   double const b3 = (a3 * one_a_sum_tmp) < CT_ ? 0. : 1.;

   double const Variation1 = coef_stencils_1_ * v2 + coef_stencils_2_ * v3 + coef_stencils_3_ * v4;
   double const Variation2 = coef_stencils_4_ * v3 + coef_stencils_5_ * v4 + coef_stencils_6_ * v5;
   double const Variation3 = coef_stencils_7_ * v1 + coef_stencils_8_ * v2 + coef_stencils_9_ * v3;

   a1 = d1_ * b1;
   a2 = d2_ * b2;
   a3 = d3_ * b3;

   double const one_a_sum = 1.0 / (a1 + a2 + a3);

   double const w1 = a1 * one_a_sum;
   double const w2 = a2 * one_a_sum;
   double const w3 = a3 * one_a_sum;

   return (w1 * Variation1 + w2 * Variation2 + w3 * Variation3) * multiplyer_stencils_;
}
