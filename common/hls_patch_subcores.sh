#!/bin/bash
#
# Copyright (C) 2014 Jens Korinth, TU Darmstadt
#
# This file is part of Tapasco (TPC).
#
# Tapasco is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Tapasco is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with Tapasco.  If not, see <http://www.gnu.org/licenses/>.
#
# @file		hls_patch_subcores.sh
# @brief	Patches the run_ippack.tcl generated by Vivado HLS to include
#               SAIF files generated during co-simulation.
# @authors	J. Korinth (jk@esa.cs.tu-darmstadt.de
#
sed -i 's/\(ipx::save_core.*\)$/if {[llength [glob -nocomplain subcore\/*]] > 0} {\n  set subcore_grp [ipx::add_file_group "subcore" $core]\n  foreach f [glob -nocomplain subcore\/*] { ipx::add_file $f $subcore_grp }\n}\n\1/g' run_ippack.tcl
sed -i '1s/^/enable_beta_device *\n/' run_ippack.tcl