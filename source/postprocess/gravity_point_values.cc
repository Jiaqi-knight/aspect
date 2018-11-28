/*
  Copyright (C) 2018 by the authors of the ASPECT code.

  This file is part of ASPECT.

  ASPECT is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  ASPEC is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ASPECT; see the file LICENSE.  If not see
  <http://www.gnu.org/licenses/>.
*/

#include <aspect/postprocess/gravity_point_values.h>
#include <aspect/geometry_model/spherical_shell.h>
#include <aspect/geometry_model/sphere.h>
#include <aspect/geometry_model/chunk.h>
#include <aspect/adiabatic_conditions/interface.h>
#include <aspect/global.h>
#include <aspect/utilities.h>

#include <deal.II/base/quadrature_lib.h>
#include <deal.II/fe/fe_values.h>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
//#include <iomanip>

namespace aspect
{
  namespace Postprocess
  {
    template <int dim>
    std::pair<std::string,std::string>
    GravityPointValues<dim>::execute (TableHandler &)
    {
      // Get quadrature formula and increase the degree of quadrature over the velocity
      // element degree.
      const QGauss<dim> quadrature_formula (this->get_fe().base_element(this->introspection().base_elements.velocities).degree+quadrature_degree_increase);
      FEValues<dim> fe_values (this->get_mapping(),
                               this->get_fe(),
                               quadrature_formula,
                               update_values   |
                               update_gradients |
                               update_quadrature_points |
                               update_JxW_values);

      // Get the value of the outer radius and inner radius
      double model_outer_radius;
      double model_inner_radius;
      if (dynamic_cast<const GeometryModel::SphericalShell<dim>*> (&this->get_geometry_model()) != nullptr)
        {
          model_outer_radius = dynamic_cast<const GeometryModel::SphericalShell<dim>&>
                               (this->get_geometry_model()).outer_radius();
          model_inner_radius = dynamic_cast<const GeometryModel::SphericalShell<dim>&>
                               (this->get_geometry_model()).inner_radius();
        }
      else if (dynamic_cast<const GeometryModel::Chunk<dim>*> (&this->get_geometry_model()) != nullptr)
        {
          model_outer_radius = dynamic_cast<const GeometryModel::Chunk<dim>&>
                               (this->get_geometry_model()).outer_radius();
          model_inner_radius = dynamic_cast<const GeometryModel::Chunk<dim>&>
                               (this->get_geometry_model()).inner_radius();
        }
      else if (dynamic_cast<const GeometryModel::Sphere<dim>*> (&this->get_geometry_model()) != nullptr)
        {
          model_outer_radius = dynamic_cast<const GeometryModel::Sphere<dim>&>
                               (this->get_geometry_model()).radius();
          model_inner_radius = 0;
        }
      else
        {
          model_outer_radius = 1;
          model_inner_radius = 0;
        }

      // Get the value of the universal gravitational constant
      const double G = aspect::constants::big_g;

      // now write all data to the file of choice. start with a pre-amble that
      // explains the meaning of the various fields
      const std::string filename = (this->get_output_directory() +
                                    "output_gravity");
      std::ofstream output (filename.c_str());
      output << "# 1: position_satellite_r" << '\n'
             << "# 2: position_satellite_phi" << '\n'
             << "# 3: position_satellite_theta" << '\n'
             << "# 4: position_satellite_x" << '\n'
             << "# 5: position_satellite_y" << '\n'
             << "# 6: position_satellite_z" << '\n'
             << "# 7: gravity_z" << '\n'
             << "# 8: gravity_z" << '\n'
             << "# 9: gravity_z" << '\n'
             << "# 10: gravity_norm" << '\n'
             << "# 11: gravity_theory" << '\n'
             << "# 12: gravity potential" << '\n'
             << "# 13: gravity_anomaly_x" << '\n'
             << "# 14: gravity_anomaly_y" << '\n'
             << "# 15: gravity_anomaly_z" << '\n'
             << "# 16: gravity_anomaly_norm" << '\n'
             << "# 17: gravity_gradient_xx" << '\n'
             << "# 18: gravity_gradient_yy" << '\n'
             << "# 19: gravity_gradient_zz" << '\n'
             << "# 20: gravity_gradient_xy" << '\n'
             << "# 21: gravity_gradient_xz" << '\n'
             << "# 22: gravity_gradient_yz" << '\n'
             << "# 23: gravity_gradient_theory_xx" << '\n'
             << "# 24: gravity_gradient_theory_yy" << '\n'
             << "# 25: gravity_gradient_theory_zz" << '\n'
             << "# 26: gravity_gradient_theory_xy" << '\n'
             << "# 27: gravity_gradient_theory_xz" << '\n'
             << "# 28: gravity_gradient_theory_yz" << '\n'
             << '\n';

      // Storing cartesian coordinates, density and JxW at local quadrature points in a vector
      // avoids to use MaterialModel and fe_values within the loops. Because the postprocessor
      // runs in parallel, the total number of local quadrature points has to be determined:
      const unsigned int n_locally_owned_cells = (this->get_triangulation().n_locally_owned_active_cells());
      const unsigned int n_quadrature_points_per_cell = quadrature_formula.size();

      // Declare the vector 'density_JxW' to store the density at quadrature points. The
      // density and the JxW are here together for simplicity in the equation (both variables
      // only appear together):
      std::vector<double> density_JxW (n_locally_owned_cells * n_quadrature_points_per_cell);
      std::vector<double> relative_density_JxW (n_locally_owned_cells * n_quadrature_points_per_cell);

      // Declare the vector 'position_point' to store the position of quadrature points:
      std::vector<Point<dim> > position_point (n_locally_owned_cells * n_quadrature_points_per_cell);

      // The following loop perform the storage of the position and density * JxW values
      // at local quadrature points:
      typename DoFHandler<dim>::active_cell_iterator
      cell = this->get_dof_handler().begin_active(),
      endc = this->get_dof_handler().end();
      MaterialModel::MaterialModelInputs<dim> in(quadrature_formula.size(),this->n_compositional_fields());
      MaterialModel::MaterialModelOutputs<dim> out(quadrature_formula.size(),this->n_compositional_fields());
      unsigned int local_cell_number = 0;
      for (; cell!=endc; ++cell)
        {
          if (cell->is_locally_owned())
            {
              fe_values.reinit (cell);
              const std::vector<Point<dim> > &position_point_cell = fe_values.get_quadrature_points();
              in.reinit(fe_values, cell, this->introspection(), this->get_solution(), false);
              this->get_material_model().evaluate(in, out);
              for (unsigned int q = 0; q < n_quadrature_points_per_cell; ++q)
                {
                  density_JxW[local_cell_number * n_quadrature_points_per_cell + q] = out.densities[q] * fe_values.JxW(q);
                  relative_density_JxW[local_cell_number * n_quadrature_points_per_cell + q] = (out.densities[q]-reference_density) * fe_values.JxW(q);
                  position_point[local_cell_number * n_quadrature_points_per_cell + q] = position_point_cell[q];
                }
              ++local_cell_number;
            }
        }
   
      // Pre-Assign the coordinates of all satellites in a vector point:
      // *** First calculate the number of satellites according to the sampling scheme:
      unsigned int n_satellites;
      if (sampling_scheme == map)
        n_satellites = n_points_radius * n_points_longitude * n_points_latitude;
      else if (sampling_scheme == list)
        n_satellites = longitude_list.size();
      else n_satellites = 1;
  
      // *** Second assign the coordinates of all satellites:
      std::vector<Point<dim> > satellites_coordinate(n_satellites);
      if (sampling_scheme == map)
        {
          unsigned int p = 0;
          for (unsigned int h=0; h < n_points_radius; ++h)
            {
              for (unsigned int i=0; i < n_points_longitude; ++i)
                {
                  for (unsigned int j=0; j < n_points_latitude; ++j)
                    {
                      satellites_coordinate[p][0] = minimum_radius + ((maximum_radius - minimum_radius) / n_points_radius) * h;
                      satellites_coordinate[p][1] = (minimum_colongitude + ((maximum_colongitude - minimum_colongitude) / n_points_longitude) * i) * numbers::PI / 180.;
                      satellites_coordinate[p][2] = (minimum_colatitude + ((maximum_colatitude - minimum_colatitude) / n_points_latitude) * j) * numbers::PI / 180.;
                      ++p;
                    }
                }
            }
        }
      if (sampling_scheme == list)
        {
          for (unsigned int p=0; p < n_satellites; ++p)
            {
              if (radius_list.size() == 1)
                satellites_coordinate[p][0] = radius_list[0];
              else
                satellites_coordinate[p][0] = radius_list[p];
              if (longitude_list[p] < 0)
                satellites_coordinate[p][1] = (360 + longitude_list[p]) * numbers::PI / 180.;
              else
                satellites_coordinate[p][1] = (longitude_list[p]) * numbers::PI / 180.;
              satellites_coordinate[p][2] = (90 - latitude_list[p]) * numbers::PI / 180. ;
            }
        }

      // This is the main loop which computes gravity acceleration, potential and
      // gradients at a point located at the spherical coordinate [r, phi, theta].
      // This loop corresponds to the 3 integrals of Newton law:
      for (unsigned int p=0; p < n_satellites; ++p)
        {

          // The spherical coordinates are shifted into cartesian to allow simplification
          // in the mathematical equation.
          std::array<double,dim> satellite_point_coordinate;
          satellite_point_coordinate[0] = satellites_coordinate[p][0];
          satellite_point_coordinate[1] = satellites_coordinate[p][1];
          satellite_point_coordinate[2] = satellites_coordinate[p][2];
          const Point<dim> position_satellite = Utilities::Coordinates::spherical_to_cartesian_coordinates<dim>(satellite_point_coordinate);

          // For each point (i.e. satellite), the fourth integral goes over cells and 
          // quadrature points to get the unique distance between those, to calculate
          // gravity vector components x,y,z (in tensor), potential and gradients.
          Tensor<1,dim> local_g;
          Tensor<1,dim> local_g_anomaly;
          Tensor<2,dim> local_g_gradient;
          double local_g_potential = 0;
          cell = this->get_dof_handler().begin_active();
          local_cell_number = 0;
          for (; cell!=endc; ++cell)
            {
              if (cell->is_locally_owned())
                {
                  for (unsigned int q = 0; q < n_quadrature_points_per_cell; ++q)
                    {
                      const double dist = (position_satellite - position_point[local_cell_number * n_quadrature_points_per_cell + q]).norm();
                      // For gravity acceleration:
                      const double KK = G * density_JxW[local_cell_number * n_quadrature_points_per_cell + q] / pow(dist,3);
                      local_g += KK * (position_satellite - position_point[local_cell_number * n_quadrature_points_per_cell + q]);
                      // For gravity anomalies:
                      const double relative_KK = G * relative_density_JxW[local_cell_number * n_quadrature_points_per_cell + q] / pow(dist,3);
                      local_g_anomaly += relative_KK * (position_satellite - position_point[local_cell_number * n_quadrature_points_per_cell + q]);
                      // For gravity potential:
                      local_g_potential -= G * density_JxW[local_cell_number * n_quadrature_points_per_cell + q] / dist;
                      // For gravity gradient:
                      const double grad_KK = G * density_JxW[local_cell_number * n_quadrature_points_per_cell + q] / pow(dist,5);
                      local_g_gradient[0][0] += grad_KK * (3.0 * pow((position_satellite[0] - position_point[local_cell_number * n_quadrature_points_per_cell + q][0]),2)
                                                               - pow(dist,2));
                      local_g_gradient[1][1] += grad_KK * (3.0 * pow((position_satellite[1] - position_point[local_cell_number * n_quadrature_points_per_cell + q][1]),2)
                                                               - pow(dist,2));
                      local_g_gradient[2][2] += grad_KK * (3.0 * pow((position_satellite[2] - position_point[local_cell_number * n_quadrature_points_per_cell + q][2]),2)
                                                               - pow(dist,2));
                      local_g_gradient[0][1] += grad_KK * (3.0 * (position_satellite[0] - position_point[local_cell_number * n_quadrature_points_per_cell + q][0])
                                                               * (position_satellite[1] - position_point[local_cell_number * n_quadrature_points_per_cell + q][1])); 
                      local_g_gradient[0][2] += grad_KK * (3.0 * (position_satellite[0] - position_point[local_cell_number * n_quadrature_points_per_cell + q][0])
                                                               * (position_satellite[2] - position_point[local_cell_number * n_quadrature_points_per_cell + q][2])); 
                      local_g_gradient[1][2] += grad_KK * (3.0 * (position_satellite[1] - position_point[local_cell_number * n_quadrature_points_per_cell + q][1])
                                                               * (position_satellite[2] - position_point[local_cell_number * n_quadrature_points_per_cell + q][2])); 
                    }
                  ++local_cell_number;
                }
             }

          // Sum local gravity components over global domain
          const Tensor<1,dim> g          = Utilities::MPI::sum (local_g, this->get_mpi_communicator());
          const Tensor<1,dim> g_anomaly  = Utilities::MPI::sum (local_g_anomaly, this->get_mpi_communicator());
          const Tensor<2,dim> g_gradient = Utilities::MPI::sum (local_g_gradient, this->get_mpi_communicator());
          const double g_potential       = Utilities::MPI::sum (local_g_potential, this->get_mpi_communicator());

          // analytical solution to calculate the theoretical gravity and gravity gradient
          // from a uniform density model. Can only be used if concentric density profile.
          double g_theory = 0;
          Tensor<2,dim> g_gradient_theory;
          if (satellites_coordinate[p][0] <= model_inner_radius)
            {
              g_theory = 0;
            }
          else if ((satellites_coordinate[p][0] > model_inner_radius) && (satellites_coordinate[p][0] < model_outer_radius))
            {
              g_theory = G * numbers::PI * 4/3 * reference_density * (satellites_coordinate[p][0] - (pow(model_inner_radius,3) / pow(satellites_coordinate[p][0],2)));
            }
          else
            {
              g_theory = G * numbers::PI * 4/3 * reference_density * (pow(model_outer_radius,3) - pow(model_inner_radius,3)) / pow(satellites_coordinate[p][0],2);
              g_gradient_theory[0][0] = -G * numbers::PI * 4/3 * reference_density * (pow(model_outer_radius,3) - pow(model_inner_radius,3))
                                                                                   * (pow(satellites_coordinate[p][0], 2) - 3.0 * pow(position_satellite[0],2))
                                                                                   /  pow(satellites_coordinate[p][0],5);
              g_gradient_theory[1][1] = -G * numbers::PI * 4/3 * reference_density * (pow(model_outer_radius,3) - pow(model_inner_radius,3))
                                                                                   * (pow(satellites_coordinate[p][0], 2) - 3.0 * pow(position_satellite[1],2))
                                                                                   /  pow(satellites_coordinate[p][0],5);
              g_gradient_theory[2][2] = -G * numbers::PI * 4/3 * reference_density * (pow(model_outer_radius,3) - pow(model_inner_radius,3))
                                                                                   * (pow(satellites_coordinate[p][0], 2) - 3.0 * pow(position_satellite[2],2))
                                                                                   /  pow(satellites_coordinate[p][0],5);
              g_gradient_theory[0][1] = -G * numbers::PI * 4/3 * reference_density * (pow(model_outer_radius,3) - pow(model_inner_radius,3))
                                                                                   * (- 3.0 * position_satellite[0] * position_satellite[1])
                                                                                   /  pow(satellites_coordinate[p][0],5);
              g_gradient_theory[0][2] = -G * numbers::PI * 4/3 * reference_density * (pow(model_outer_radius,3) - pow(model_inner_radius,3))
                                                                                   * (- 3.0 * position_satellite[0] * position_satellite[2])
                                                                                   /  pow(satellites_coordinate[p][0],5);
              g_gradient_theory[1][2] = -G * numbers::PI * 4/3 * reference_density * (pow(model_outer_radius,3) - pow(model_inner_radius,3))
                                                                                   * (- 3.0 * position_satellite[1] * position_satellite[2])
                                                                                   /  pow(satellites_coordinate[p][0],5);
            }

          // write output. 
          // g_gradient is here given in eotvos E (1E = 1e-9 per square seconds) 
          if (dealii::Utilities::MPI::this_mpi_process(this->get_mpi_communicator()) == 0)
            {
              output << satellites_coordinate[p][0] << ' '
                     << satellites_coordinate[p][1] *180. / numbers::PI << ' '
                     << satellites_coordinate[p][2] *180. / numbers::PI << ' '
                     << position_satellite[0] << ' '
                     << position_satellite[1] << ' '
                     << position_satellite[2] << ' '
                     << std::setprecision(18) << g << ' '
                     << std::setprecision(18) << g.norm() << ' '
                     << std::setprecision(18) << g_theory << ' '
                     << std::setprecision(9) << g_potential << ' '
                     << std::setprecision(9) << g_anomaly << ' '
                     << std::setprecision(9) << g_anomaly.norm() << ' '
                     << g_gradient[0][0] *1e9 << ' '
                     << g_gradient[1][1] *1e9 << ' '
                     << g_gradient[2][2] *1e9 << ' '
                     << g_gradient[0][1] *1e9 << ' '
                     << g_gradient[0][2] *1e9 << ' '
                     << g_gradient[1][2] *1e9 << ' '
                     << g_gradient_theory[0][0] *1e9 << ' '
                     << g_gradient_theory[1][1] *1e9 << ' '
                     << g_gradient_theory[2][2] *1e9 << ' '
                     << g_gradient_theory[0][1] *1e9 << ' '
                     << g_gradient_theory[0][2] *1e9 << ' '
                     << g_gradient_theory[1][2] *1e9 << ' '
                     << '\n';
            }
        }
      return std::pair<std::string, std::string> ("gravity computation file:",filename);
    }
     
    template <int dim>
    void
    GravityPointValues<dim>::declare_parameters (ParameterHandler &prm)
    {
      prm.enter_subsection ("Postprocess");
      {
        prm.enter_subsection ("Gravity calculation");
        {
          prm.declare_entry ("Sampling scheme", "map",
                             Patterns::Selection ("map|list"),
                             "Choose which sampling scheme. A map is limited between "
                             "a minimum and maximum radius, longitude and latitude. A "
                             "A list contains specific coordinates of the satellites.");
          prm.declare_entry ("Quadrature degree increase", "0",
                             Patterns::Double (0.0),
                             "Quadrature degree increase over the velocity element "
                             "degree may be required when gravity is calculated near "
                             "the surface or inside the model. An increase in the "
                             "quadrature element adds accuracy to the gravity "
                             "solution from noise due to the model grid.");
          prm.declare_entry ("Number points radius", "1",
                             Patterns::Double (0.0),
                             "Gravity may be calculated for a set of points along "
                             "the radius (e.g. depth profile) between a minimum and "
                             "maximum radius.");
          prm.declare_entry ("Number points longitude", "1",
                             Patterns::Double (0.0),
                             "Gravity may be calculated for a sets of points along "
                             "the longitude (e.g. gravity map) between a minimum and "
                             "maximum longitude.");
          prm.declare_entry ("Number points latitude", "1",
                             Patterns::Double (0.0),
                             "Gravity may be calculated for a sets of points along "
                             "the latitude (e.g. gravity map) between a minimum and "
                             "maximum latitude.");
          prm.declare_entry ("Minimum radius", "0",
                             Patterns::Double (0.0),
                             "Minimum radius may be defined in or outside the model. "
                             "Prescribe a minimum radius for a sampling coverage at a "
                             "specific height.");
          prm.declare_entry ("Maximum radius", "0",
                             Patterns::Double (0.0),
                             "Maximum radius can be defined in or outside the model.");
          prm.declare_entry ("Minimum longitude", "-180",
                             Patterns::Double (-180.0,180.0),
                             "Gravity may be calculated for a sets of points along "
                             "the longitude between a minimum and maximum longitude.");
          prm.declare_entry ("Minimum latitude", "-90",
                             Patterns::Double (-90.0,90.0),
                             "Gravity may be calculated for a sets of points along "
                             "the latitude between a minimum and maximum latitude.");
          prm.declare_entry ("Maximum longitude", "180",
                             Patterns::Double (-180.0,180.0),
                             "Gravity may be calculated for a sets of points along "
                             "the longitude between a minimum and maximum longitude.");
          prm.declare_entry ("Maximum latitude", "90",
                             Patterns::Double (-90.0,90.0),
                             "Gravity may be calculated for a sets of points along "
                             "the latitude between a minimum and maximum latitude.");
          prm.declare_entry ("Reference density", "3300",
                             Patterns::Double (0.0),
                             "Gravity anomalies are computed using densities anomalies "
                             "relative to a reference density.");
          prm.declare_entry ("List of radius", "",
                             Patterns::List (Patterns::Double(0)),
                             "List of satellite radius coordinates. Just specify one "
                             "radius if all points values have the same radius. If "
                             "not, make sure there are as many radius as longitude "
                             "and latitude");
          prm.declare_entry ("List of longitude", "",
                             Patterns::List (Patterns::Double(-180.0,180.0)),
                             "List of satellite longitude coordinates.");
          prm.declare_entry ("List of latitude", "",
                             Patterns::List (Patterns::Double(-90.0,90.0)),
                             "List of satellite latitude coordinates.");
        
        }
        prm.leave_subsection();
      }
      prm.leave_subsection();
    }

    template <int dim>
    void
    GravityPointValues<dim>::parse_parameters (ParameterHandler &prm)
    {
      prm.enter_subsection ("Postprocess");
      {
        prm.enter_subsection ("Gravity calculation");
        {
          if (prm.get ("Sampling scheme") == "map")
            sampling_scheme = map;
          else if (prm.get ("Sampling scheme") == "list")
	    sampling_scheme = list;
          else
            AssertThrow (false, ExcMessage ("Not a valid sampling scheme."));
          quadrature_degree_increase = prm.get_double ("Quadrature degree increase");
          n_points_radius    = prm.get_double ("Number points radius");
          n_points_longitude = prm.get_double ("Number points longitude");
          n_points_latitude  = prm.get_double ("Number points latitude");
          minimum_radius    = prm.get_double ("Minimum radius");
          maximum_radius    = prm.get_double ("Maximum radius");
          minimum_colongitude = prm.get_double ("Minimum longitude") + 180;
          maximum_colongitude = prm.get_double ("Maximum longitude") + 180;
          minimum_colatitude  = prm.get_double ("Minimum latitude") + 90;
          maximum_colatitude  = prm.get_double ("Maximum latitude") + 90;
          reference_density = prm.get_double ("Reference density");
          radius_list    = Utilities::string_to_double(Utilities::split_string_list(prm.get("List of radius")));
          longitude_list = Utilities::string_to_double(Utilities::split_string_list(prm.get("List of longitude")));
          latitude_list  = Utilities::string_to_double(Utilities::split_string_list(prm.get("List of latitude")));
	  AssertThrow (longitude_list.size() == latitude_list.size(),
                       ExcMessage ("Make sure you have the same number of point coordinates in the list sampling scheme."));
          AssertThrow (dynamic_cast<const GeometryModel::SphericalShell<dim>*> (&this->get_geometry_model()) != nullptr ||
                       dynamic_cast<const GeometryModel::Sphere<dim>*> (&this->get_geometry_model()) != nullptr ||
                       dynamic_cast<const GeometryModel::Chunk<dim>*> (&this->get_geometry_model()) != nullptr,
                       ExcMessage ("This postprocessor can only be used if the geometry is a sphere, spherical shell or spherical chunk."));
          AssertThrow (! this->get_material_model().is_compressible(),
                       ExcMessage("This postprocessor was only tested for incompressible models so far."));
          AssertThrow (dim==3,
                       ExcMessage("This postprocessor was only tested for 3D models."));
        }
        prm.leave_subsection();
      }
      prm.leave_subsection();
    }

  }
}


// explicit instantiations
namespace aspect
{
  namespace Postprocess
  {
    ASPECT_REGISTER_POSTPROCESSOR(GravityPointValues,
                                  "gravity calculation",
                                  "A postprocessor that computes gravity, gravity anomalies, gravity "
                                  "potential and gravity gradients for a set of points (e.g. satellites) "
                                  "in or above the model surface for either a user-defined range of "
                                  "latitudes, longitudes and radius or a list of point coordinates."
                                  "Spherical coordinates in the output file are radius, colatitude "
                                  "and colongitude.")
  }
}
