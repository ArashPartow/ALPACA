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
#ifndef NOBLE_ABEL_STIFFENED_GAS_H
#define NOBLE_ABEL_STIFFENED_GAS_H

#include <limits>

#include "materials/material.h"

/**
 * @brief The NobleAbelStiffenedGas class implements a generic NASG equation of state, i.e. material parameters must be set via caller input.
 *        The equation of state is described in \cite LeMetayer2016.
 */
class NobleAbelStiffenedGas : public Material {

   double const epsilon_ = std::numeric_limits<double>::epsilon();
   double const gamma_;
   double const covolume_;
   double const pressure_constant_;
   double const energy_constant_;
   double const specific_heat_capacity_;
   double const thermal_conductivity_;
   double const mu_shear_;
   double const mu_bulk_;

   double DoGetPressure   ( double const density, double const momentum_x, double const momentum_y, double const momentum_z, double const energy ) const override;
   double DoGetEnthalpy   ( double const density, double const momentum_x, double const momentum_y, double const momentum_z, double const energy ) const override;
   double DoGetEnergy     ( double const density, double const momentum_x, double const momentum_y, double const momentum_z, double const pressure ) const override;
   double DoGetTemperature( double const density, double const momentum_x, double const momentum_y, double const momentum_z, double const energy ) const override;

   double DoGetGruneisen() const override;
   double DoGetGruneisen( double const density ) const override;
   double DoGetPsi( double const pressure, double const one_density ) const override;
   double DoGetGamma() const override;
   double DoGetB() const override;
   double DoGetSpeedOfSound( double const density, double const pressure ) const override;

   std::vector<double> DoGetViscosity() const override;
   double DoGetThermalConductivity() const override;
   double DoGetSpecificHeat() const override;

public:
   NobleAbelStiffenedGas() = delete;
   explicit NobleAbelStiffenedGas( double const gamma, double const covolume, double const pressure_constant, double const energy_constant, double const specific_heat_capacity, 
      double const thermal_conductivity, double const mu_shear, double const mu_bulk );
   virtual ~NobleAbelStiffenedGas() = default;
   NobleAbelStiffenedGas( NobleAbelStiffenedGas const& ) = delete;
   NobleAbelStiffenedGas& operator=( NobleAbelStiffenedGas const& ) = delete;
   NobleAbelStiffenedGas( NobleAbelStiffenedGas&& ) = delete;
   NobleAbelStiffenedGas operator=( NobleAbelStiffenedGas&& ) = delete;

};

#endif // NOBLE_ABEL_STIFFENED_GAS_H
