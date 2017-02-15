/*
  Copyright (c) 2016, 2017, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA
*/

#include "sql/gis/srs/srs.h"

#include <boost/variant/get.hpp>
#include <stddef.h>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "m_ctype.h"                       // my_strcasecmp
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysqld_error.h"                  // ER_*
#include "wkt_parser.h"

/**
  Extract projection parameter values from the parse tree and assign
  them to variables.

  The function is given a list of EPSG parameter codes for all
  parameters that can be extracted, and pointers to the variable where
  each parameter should be stored.

  Mandatory parameters must be set to NAN before calling this
  function. Optional parameters must be set to their default value.

  If a mandatory parameter is missing, an error is flagged and the
  function returns true.

  @param[in] srid The spatial reference system ID, used in error reporting
  @param[in] proj Parse tree
  @param[in,out] params List of mandatory parameters (EPSG codes) and
  pointers to where their values should be stored.

  @retval true An error has occurred. The error has been flagged.
  @retval false Success
*/
static bool set_parameters(srid_t srid,
                           gis::srs::wkt_parser::Projected_cs *proj,
                           std::vector<std::pair<int, double *>> *params)
{
  std::map<int, std::string> param_names;
  param_names[1026]= "c1";
  param_names[1027]= "c2";
  param_names[1028]= "c3";
  param_names[1029]= "c4";
  param_names[1030]= "c5";
  param_names[1031]= "c6";
  param_names[1032]= "c7";
  param_names[1033]= "c8";
  param_names[1034]= "c9";
  param_names[1035]= "c10";
  param_names[1036]= "azimuth";
  param_names[1038]= "ellipsoid_scale_factor";
  param_names[1039]= "projection_plane_height_at_origin";
  param_names[8617]= "evaluation_point_ordinate_1";
  param_names[8618]= "evaluation_point_ordinate_2";
  param_names[8801]= "latitude_of_origin";
  param_names[8802]= "central_meridian";
  param_names[8805]= "scale_factor";
  param_names[8806]= "false_easting";
  param_names[8807]= "false_northing";
  param_names[8811]= "latitude_of_center";
  param_names[8812]= "longitude_of_center";
  param_names[8813]= "azimuth";
  param_names[8814]= "rectified_grid_angle";
  param_names[8815]= "scale_factor";
  param_names[8816]= "false_easting";
  param_names[8817]= "false_northing";
  param_names[8818]= "pseudo_standard_parallel_1";
  param_names[8819]= "scale_factor";
  param_names[8821]= "latitude_of_origin";
  param_names[8822]= "central_meridian";
  param_names[8823]= "standard_parallel_1";
  param_names[8824]= "standard_parallel_2";
  param_names[8826]= "false_easting";
  param_names[8827]= "false_northing";
  param_names[8830]= "initial_longitude";
  param_names[8831]= "zone_width";
  param_names[8832]= "standard_parallel";
  param_names[8833]= "longitude_of_center";

  std::map<int, std::string> param_aliases;
  param_aliases[8823]= "standard_parallel1";
  param_aliases[8824]= "standard_parallel2";

  /*
    Loop through parameters in the parse tree one by one. For each
    parameter, do this:

    If the parameter has an authority clause with an EPSG code, and
    the authority code matches a code in the list of required
    parameters, assign the value to the parameter variable.

    If there is no authority clause, or the authority is not EPSG,
    check if the name of the parameter matches the name or alias of a
    required parameter. If it does, assign the value to the parameter
    variable.

    Otherwise, ignore the parameter.

    In other words: If a parameter has an EPSG authority code, obey
    it. If not, use the parameter name.
  */
  for (auto i= proj->parameters.begin(); i != proj->parameters.end(); i++)
  {
    for (size_t j= 0; j < params->size(); j++)
    {
      if (!my_strcasecmp(&my_charset_latin1, "EPSG", i->authority.name.c_str()))
      {
        if (!my_strcasecmp(&my_charset_latin1,
                           std::to_string(params->at(j).first).c_str(),
                           i->authority.code.c_str()))
        {
          *(params->at(j).second)= i->value;
        }
      }
      else if (!my_strcasecmp(&my_charset_latin1,
                              param_names[params->at(j).first].c_str(),
                              i->name.c_str()))
      {
        *(params->at(j).second)= i->value;
      }
      else if (!my_strcasecmp(&my_charset_latin1,
                              param_aliases[params->at(j).first].c_str(),
                              i->name.c_str()))
      {
        *(params->at(j).second)= i->value;
      }
    }
  }

  // All mandatory parameters are set to NAN before calling this
  // function. If any parameters are still NAN, raise an exception
  // condition.
  for (size_t i= 0; i < params->size(); i++)
  {
    if (std::isnan(*(params->at(i).second)))
    {
      int epsg_code= params->at(i).first;
      my_error(ER_SRS_PROJ_PARAMETER_MISSING, MYF(0), srid,
               param_names[epsg_code].c_str(), epsg_code);
      return true;
    }
  }

  return false;
}


namespace gis { namespace srs {


bool Geographic_srs::init(srid_t, gis::srs::wkt_parser::Geographic_cs *g)
{
  m_semi_major_axis= g->datum.spheroid.semi_major_axis;
  m_inverse_flattening= g->datum.spheroid.inverse_flattening;

  // Semi-major axis and inverse flattening are required by the parser.
  DBUG_ASSERT(!std::isnan(m_semi_major_axis));
  DBUG_ASSERT(!std::isnan(m_inverse_flattening));

  if (g->datum.towgs84.valid)
  {
    m_towgs84[0]= g->datum.towgs84.dx;
    m_towgs84[1]= g->datum.towgs84.dy;
    m_towgs84[2]= g->datum.towgs84.dz;
    m_towgs84[3]= g->datum.towgs84.ex;
    m_towgs84[4]= g->datum.towgs84.ey;
    m_towgs84[5]= g->datum.towgs84.ez;
    m_towgs84[6]= g->datum.towgs84.ppm;

    // If not all parameters are used, the parser sets the remaining ones to 0.
    DBUG_ASSERT(!std::isnan(m_towgs84[0]));
    DBUG_ASSERT(!std::isnan(m_towgs84[1]));
    DBUG_ASSERT(!std::isnan(m_towgs84[2]));
    DBUG_ASSERT(!std::isnan(m_towgs84[3]));
    DBUG_ASSERT(!std::isnan(m_towgs84[4]));
    DBUG_ASSERT(!std::isnan(m_towgs84[5]));
    DBUG_ASSERT(!std::isnan(m_towgs84[6]));
  }

  m_prime_meridian= g->prime_meridian.longitude;
  m_angular_unit= g->angular_unit.conversion_factor;

  // Prime meridian and angular unit are required by the parser.
  DBUG_ASSERT(!std::isnan(m_prime_meridian));
  DBUG_ASSERT(!std::isnan(m_angular_unit));

  if (g->axes.valid)
  {
    m_axes[0]= g->axes.x.direction;
    m_axes[1]= g->axes.y.direction;

    // The parser requires either both or none to be specified.
    DBUG_ASSERT(m_axes[0] != Axis_direction::UNSPECIFIED);
    DBUG_ASSERT(m_axes[1] != Axis_direction::UNSPECIFIED);
  }

  return false;
}


bool Projected_srs::init(srid_t srid, gis::srs::wkt_parser::Projected_cs *p)
{
  bool res= false;

  res= m_geographic_srs.init(srid, &p->geographic_cs);

  m_linear_unit= p->linear_unit.conversion_factor;

  // Linear unit is required by the parser.
  DBUG_ASSERT(!std::isnan(m_linear_unit));

  if (p->axes.valid)
  {
    m_axes[0]= p->axes.x.direction;
    m_axes[1]= p->axes.y.direction;

    // The parser requires either both or none to be specified.
    DBUG_ASSERT(m_axes[0] != Axis_direction::UNSPECIFIED);
    DBUG_ASSERT(m_axes[1] != Axis_direction::UNSPECIFIED);
  }

  return res;
}


bool Unknown_projected_srs::init(srid_t srid,
                                 gis::srs::wkt_parser::Projected_cs *p)
{
  return Projected_srs::init(srid, p);
}


bool Popular_visualisation_pseudo_mercator_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_azimuthal_equal_area_spherical_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Equidistant_cylindrical_srs::init(srid_t srid,
                                       gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Equidistant_cylindrical_spherical_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Krovak_north_orientated_srs::init(srid_t srid,
                                       gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8811, &m_latitude_of_center));
  params.push_back(std::make_pair(8833, &m_longitude_of_center));
  params.push_back(std::make_pair(1036, &m_azimuth));
  params.push_back(std::make_pair(8818, &m_pseudo_standard_parallel_1));
  params.push_back(std::make_pair(8819, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Krovak_modified_srs::init(srid_t srid,
                               gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8811, &m_latitude_of_center));
  params.push_back(std::make_pair(8833, &m_longitude_of_center));
  params.push_back(std::make_pair(1036, &m_azimuth));
  params.push_back(std::make_pair(8818, &m_pseudo_standard_parallel_1));
  params.push_back(std::make_pair(8819, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));
  params.push_back(std::make_pair(8617, &m_evaluation_point_ordinate_1));
  params.push_back(std::make_pair(8618, &m_evaluation_point_ordinate_2));
  params.push_back(std::make_pair(1026, &m_c1));
  params.push_back(std::make_pair(1027, &m_c2));
  params.push_back(std::make_pair(1028, &m_c3));
  params.push_back(std::make_pair(1029, &m_c4));
  params.push_back(std::make_pair(1030, &m_c5));
  params.push_back(std::make_pair(1031, &m_c6));
  params.push_back(std::make_pair(1032, &m_c7));
  params.push_back(std::make_pair(1033, &m_c8));
  params.push_back(std::make_pair(1034, &m_c9));
  params.push_back(std::make_pair(1035, &m_c10));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Krovak_modified_north_orientated_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8811, &m_latitude_of_center));
  params.push_back(std::make_pair(8833, &m_longitude_of_center));
  params.push_back(std::make_pair(1036, &m_azimuth));
  params.push_back(std::make_pair(8818, &m_pseudo_standard_parallel_1));
  params.push_back(std::make_pair(8819, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));
  params.push_back(std::make_pair(8617, &m_evaluation_point_ordinate_1));
  params.push_back(std::make_pair(8618, &m_evaluation_point_ordinate_2));
  params.push_back(std::make_pair(1026, &m_c1));
  params.push_back(std::make_pair(1027, &m_c2));
  params.push_back(std::make_pair(1028, &m_c3));
  params.push_back(std::make_pair(1029, &m_c4));
  params.push_back(std::make_pair(1030, &m_c5));
  params.push_back(std::make_pair(1031, &m_c6));
  params.push_back(std::make_pair(1032, &m_c7));
  params.push_back(std::make_pair(1033, &m_c8));
  params.push_back(std::make_pair(1034, &m_c9));
  params.push_back(std::make_pair(1035, &m_c10));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_conic_conformal_2sp_michigan_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8821, &m_latitude_of_origin));
  params.push_back(std::make_pair(8822, &m_longitude_of_origin));
  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8824, &m_standard_parallel_2));
  params.push_back(std::make_pair(8826, &m_false_easting));
  params.push_back(std::make_pair(8827, &m_false_northing));
  params.push_back(std::make_pair(1038, &m_ellipsoid_scale_factor));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Colombia_urban_srs::init(srid_t srid,
                              gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));
  params.push_back(std::make_pair(1039, &m_projection_plane_height_at_origin));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_conic_conformal_1sp_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_conic_conformal_2sp_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8821, &m_latitude_of_origin));
  params.push_back(std::make_pair(8822, &m_longitude_of_origin));
  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8824, &m_standard_parallel_2));
  params.push_back(std::make_pair(8826, &m_false_easting));
  params.push_back(std::make_pair(8827, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_conic_conformal_2sp_belgium_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8821, &m_latitude_of_origin));
  params.push_back(std::make_pair(8822, &m_longitude_of_origin));
  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8824, &m_standard_parallel_2));
  params.push_back(std::make_pair(8826, &m_false_easting));
  params.push_back(std::make_pair(8827, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Mercator_variant_a_srs::init(srid_t srid,
                                  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Mercator_variant_b_srs::init(srid_t srid,
                                  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Cassini_soldner_srs::init(srid_t srid,
                               gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Transverse_mercator_srs::init(srid_t srid,
                                   gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Transverse_mercator_south_orientated_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Oblique_stereographic_srs::init(srid_t srid,
                                     gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Polar_stereographic_variant_a_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool New_zealand_map_grid_srs::init(srid_t srid,
                                    gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Hotine_oblique_mercator_variant_a_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8811, &m_latitude_of_center));
  params.push_back(std::make_pair(8812, &m_longitude_of_center));
  params.push_back(std::make_pair(8813, &m_azimuth));
  params.push_back(std::make_pair(8814, &m_rectified_grid_angle));
  params.push_back(std::make_pair(8815, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Laborde_oblique_mercator_srs::init(srid_t srid,
                                        gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8811, &m_latitude_of_center));
  params.push_back(std::make_pair(8812, &m_longitude_of_center));
  params.push_back(std::make_pair(8813, &m_azimuth));
  params.push_back(std::make_pair(8815, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Hotine_oblique_mercator_variant_b_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8811, &m_latitude_of_center));
  params.push_back(std::make_pair(8812, &m_longitude_of_center));
  params.push_back(std::make_pair(8813, &m_azimuth));
  params.push_back(std::make_pair(8814, &m_rectified_grid_angle));
  params.push_back(std::make_pair(8815, &m_scale_factor));
  params.push_back(std::make_pair(8816, &m_false_easting));
  params.push_back(std::make_pair(8817, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Tunisia_mining_grid_srs::init(srid_t srid,
                                   gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8821, &m_latitude_of_origin));
  params.push_back(std::make_pair(8822, &m_longitude_of_origin));
  params.push_back(std::make_pair(8826, &m_false_easting));
  params.push_back(std::make_pair(8827, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_conic_near_conformal_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool American_polyconic_srs::init(srid_t srid,
                                  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Krovak_srs::init(srid_t srid, gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8811, &m_latitude_of_center));
  params.push_back(std::make_pair(8833, &m_longitude_of_center));
  params.push_back(std::make_pair(1036, &m_azimuth));
  params.push_back(std::make_pair(8818, &m_pseudo_standard_parallel_1));
  params.push_back(std::make_pair(8819, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_azimuthal_equal_area_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Albers_equal_area_srs::init(srid_t srid,
                                 gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8821, &m_latitude_of_origin));
  params.push_back(std::make_pair(8822, &m_longitude_of_origin));
  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8824, &m_standard_parallel_2));
  params.push_back(std::make_pair(8826, &m_false_easting));
  params.push_back(std::make_pair(8827, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Transverse_mercator_zoned_grid_system_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8830, &m_initial_longitude));
  params.push_back(std::make_pair(8831, &m_zone_width));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_conic_conformal_west_orientated_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8805, &m_scale_factor));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Bonne_south_orientated_srs::init(srid_t srid,
                                      gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Polar_stereographic_variant_b_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8832, &m_standard_parallel));
  params.push_back(std::make_pair(8833, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Polar_stereographic_variant_c_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8832, &m_standard_parallel));
  params.push_back(std::make_pair(8833, &m_longitude_of_origin));
  params.push_back(std::make_pair(8826, &m_false_easting));
  params.push_back(std::make_pair(8827, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Guam_projection_srs::init(srid_t srid,
                               gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Modified_azimuthal_equidistant_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Hyperbolic_cassini_soldner_srs::init(srid_t srid,
                                          gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8801, &m_latitude_of_origin));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_cylindrical_equal_area_spherical_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}


bool Lambert_cylindrical_equal_area_srs::init(
  srid_t srid,
  gis::srs::wkt_parser::Projected_cs *p)
{
  std::vector<std::pair<int, double *>> params;

  params.push_back(std::make_pair(8823, &m_standard_parallel_1));
  params.push_back(std::make_pair(8802, &m_longitude_of_origin));
  params.push_back(std::make_pair(8806, &m_false_easting));
  params.push_back(std::make_pair(8807, &m_false_northing));

  bool res= Projected_srs::init(srid, p);
  res|= set_parameters(srid, p, &params);

  return res;
}

}} // gis::srs


/**
  Create a geographic SRS description from a parse tree.

  @param[in] srid Spatial reference system ID to use when reporting errors
  @param[in] geog Parser output
  @param[in,out] srs Geographic SRS object allocated by the caller

  @retval true An error has occurred
  @retval false Success
*/
static bool create_geographic_srs(srid_t srid,
                                  gis::srs::wkt_parser::Geographic_cs *geog,
                                  gis::srs::Geographic_srs **srs)
{
  *srs= new gis::srs::Geographic_srs();

  return (*srs)->init(srid, geog);
}


/**
  Create a new projected SRS object based on EPSG code.

  When creating a projected SRS object for a projection without an
  EPSG code, code 0 should be used.

  If the EPSG code is 0 or unkown, an Unkown_projected_srs object is
  returned.

  @param epsg_code EPSG coordinate operation method (i.e. projection
                   type) code

  @return New projected SRS object. The caller is responsible for
          deleting it.
*/
static gis::srs::Projected_srs *new_projection(int epsg_code)
{
  switch (epsg_code)
  {
  case 0:
    return new gis::srs::Unknown_projected_srs();
  case 1024:
    return new gis::srs::Popular_visualisation_pseudo_mercator_srs();
  case 1027:
    return new gis::srs::Lambert_azimuthal_equal_area_spherical_srs();
  case 1028:
    return new gis::srs::Equidistant_cylindrical_srs();
  case 1029:
    return new gis::srs::Equidistant_cylindrical_spherical_srs();
  case 1041:
    return new gis::srs::Krovak_north_orientated_srs();
  case 1042:
    return new gis::srs::Krovak_modified_srs();
  case 1043:
    return new gis::srs::Krovak_modified_north_orientated_srs();
  case 1051:
    return new gis::srs::Lambert_conic_conformal_2sp_michigan_srs();
  case 1052:
    return new gis::srs::Colombia_urban_srs();
  case 9801:
    return new gis::srs::Lambert_conic_conformal_1sp_srs();
  case 9802:
    return new gis::srs::Lambert_conic_conformal_2sp_srs();
  case 9803:
    return new gis::srs::Lambert_conic_conformal_2sp_belgium_srs();
  case 9804:
    return new gis::srs::Mercator_variant_a_srs();
  case 9805:
    return new gis::srs::Mercator_variant_b_srs();
  case 9806:
    return new gis::srs::Cassini_soldner_srs();
  case 9807:
    return new gis::srs::Transverse_mercator_srs();
  case 9808:
    return new gis::srs::Transverse_mercator_south_orientated_srs();
  case 9809:
    return new gis::srs::Oblique_stereographic_srs();
  case 9810:
    return new gis::srs::Polar_stereographic_variant_a_srs();
  case 9811:
    return new gis::srs::New_zealand_map_grid_srs();
  case 9812:
    return new gis::srs::Hotine_oblique_mercator_variant_a_srs();
  case 9813:
    return new gis::srs::Laborde_oblique_mercator_srs();
  case 9815:
    return new gis::srs::Hotine_oblique_mercator_variant_b_srs();
  case 9816:
    return new gis::srs::Tunisia_mining_grid_srs();
  case 9817:
    return new gis::srs::Lambert_conic_near_conformal_srs();
  case 9818:
    return new gis::srs::American_polyconic_srs();
  case 9819:
    return new gis::srs::Krovak_srs();
  case 9820:
    return new gis::srs::Lambert_azimuthal_equal_area_srs();
  case 9822:
    return new gis::srs::Albers_equal_area_srs();
  case 9824:
    return new gis::srs::Transverse_mercator_zoned_grid_system_srs();
  case 9826:
    return new gis::srs::Lambert_conic_conformal_west_orientated_srs();
  case 9828:
    return new gis::srs::Bonne_south_orientated_srs();
  case 9829:
    return new gis::srs::Polar_stereographic_variant_b_srs();
  case 9830:
    return new gis::srs::Polar_stereographic_variant_c_srs();
  case 9831:
    return new gis::srs::Guam_projection_srs();
  case 9832:
    return new gis::srs::Modified_azimuthal_equidistant_srs();
  case 9833:
    return new gis::srs::Hyperbolic_cassini_soldner_srs();
  case 9834:
    return new gis::srs::Lambert_cylindrical_equal_area_spherical_srs();
  case 9835:
    return new gis::srs::Lambert_cylindrical_equal_area_srs();
  default:
    return new gis::srs::Unknown_projected_srs();
  }
}


/**
  Create a projected SRS description from a parse tree.

  @param[in] srid Spatial reference system ID to use when reporting errors
  @param[in] proj Parser output
  @param[out] srs A newly allocated projected SRS object

  @retval true An error has occurred
  @retval false Success
*/
static bool create_projected_srs(srid_t srid,
                                 gis::srs::wkt_parser::Projected_cs *proj,
                                 gis::srs::Projected_srs **srs)
{
  int epsg_code= 0;
  if (!my_strcasecmp(&my_charset_latin1, "EPSG",
                     proj->projection.authority.name.c_str()))
  {
    try
    {
      epsg_code= std::stoi(proj->projection.authority.code);
    }
    catch (...) // Invalid or out of range.
    {
      epsg_code= 0;
    }
  }

  *srs= new_projection(epsg_code);

  return (*srs)->init(srid, proj);
}


bool gis::srs::parse_wkt(srid_t srid, const char *begin, const char *end,
                         Spatial_reference_system **result)
{
  if (begin == nullptr || begin == end)
  {
    my_error(ER_SRS_PARSE_ERROR, MYF(0), srid);
    return true;
  }

  namespace wp = gis::srs::wkt_parser;

  wp::Coordinate_system cs;
  bool res= wp::parse_wkt(srid, begin, end, &cs);

  if (!res)
  {
    if (wp::Projected_cs *proj= boost::get<wp::Projected_cs>(&cs))
    {
      Projected_srs *tmp= nullptr;
      res= create_projected_srs(srid, proj, &tmp);
      if (res && tmp != nullptr)
      {
        delete tmp;
        tmp= nullptr;
      }
      *result= tmp;
    }
    if (wp::Geographic_cs *geog= boost::get<wp::Geographic_cs>(&cs))
    {
      Geographic_srs *tmp= nullptr;
      res= create_geographic_srs(srid, geog, &tmp);
      if (res && tmp != nullptr)
      {
        delete tmp;
        tmp= nullptr;
      }
      *result= tmp;
    }
  }

  return res;
}
