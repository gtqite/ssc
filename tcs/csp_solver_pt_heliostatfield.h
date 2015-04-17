#ifndef __csp_solver_pt_heliostatfield_
#define __csp_solver_pt_heliostatfield_

#include "csp_solver_util.h"
#include "csp_solver_core.h"

#include "sort_method.h"
#include "interpolation_routines.h"
#include "AutoPilot_API.h" 
#include "IOUtil.h"


class C_pt_heliostatfield
{
private:
	// Class Instances
	GaussMarkov *field_efficiency_table;
	MatDoub m_flux_positions;
	sp_flux_table fluxtab;
	
	double m_p_start;
	double m_p_track;
	double m_hel_stow_deploy;
	double m_v_wind_max;

	int m_n_flux_x;
	int m_n_flux_y;
	int m_N_hel;

	//Stored Variables
	double m_eta_prev;
	double m_v_wind_prev;
	double m_v_wind_current;

	// member string for exception messages
	std::string error_msg;

	double rdist(VectDoub *p1, VectDoub *p2, int dim = 2);

public:
	// Class to save messages for up stream classes
	C_csp_messages mc_csp_messages;

	C_pt_heliostatfield();

	~C_pt_heliostatfield();

	struct RUN_TYPE { enum A {AUTO, USER_FIELD, USER_DATA}; };

	// Callback funtion
	bool(*mf_callback)(simulation_info* siminfo, void *data);
	void *m_cdata;

	struct S_params
	{
		int m_run_type;
		double m_helio_width;
		double m_helio_height;
		double m_helio_optical_error;
		double m_helio_active_fraction;
		double m_dens_mirror;
		double m_helio_reflectance;
		double m_rec_absorptance;
		double m_rec_height;
		double m_rec_aspect;
		double m_rec_hl_perm2;
		double m_q_design;
		double m_h_tower;
		std::string m_weather_file;
		int m_land_bound_type;
		double m_land_max;
		double m_land_min;

		//double *m_land_bound_table;
		//int m_nrows_land_bound_table;
		//int m_ncols_land_bound_table;
		util::matrix_t<double> m_land_bound_table;

		//double *m_land_bound_list;
		//int m_nrows_land_bound_list;
		util::matrix_t<double> m_land_bound_list;

		double m_p_start;
		double m_p_track;
		double m_hel_stow_deploy;
		double m_v_wind_max;
		double m_interp_nug;
		double m_interp_beta;

		int m_n_flux_x;
		int m_n_flux_y;

		//double *m_helio_positions;
		//int m_N_hel;
		//int m_pos_dim;
		util::matrix_t<double> m_helio_positions;

		//double *m_helio_aim_points;
		//int m_nrows_helio_aim_points;
		//int m_ncols_helio_aim_points;
		util::matrix_t<double> m_helio_aim_points;

		//double *m_eta_map;
		//int m_nrows_eta_map;
		//int m_ncols_eta_map;
		util::matrix_t<double> m_eta_map;

		//double *m_flux_positions;
		//int m_nfluxpos;
		//int m_nfposdim;
		util::matrix_t<double> m_flux_positions;

		//double *m_flux_maps;
		//int m_nfluxmap;
		//int m_nfluxcol;
		util::matrix_t<double> m_flux_maps;

		double m_c_atm_0;
		double m_c_atm_1;
		double m_c_atm_2;
		double m_c_atm_3;
		
		int m_n_facet_x;
		int m_n_facet_y;
		int m_cant_type;
		int m_focus_type;
		int m_n_flux_days;
		int m_delta_flux_hrs;

		double m_dni_des;
		double m_land_area;

		S_params()
		{
			// Integers
			m_run_type = m_land_bound_type = /*m_nrows_land_bound_table = m_ncols_land_bound_table =*/ /* m_nrows_land_bound_list =*/ m_n_flux_x = m_n_flux_y = /*m_N_hel = m_pos_dim = */
				/*m_nrows_helio_aim_points = m_ncols_helio_aim_points =*/ /*m_nrows_eta_map = m_ncols_eta_map =*/ /*m_nfluxpos = m_nfposdim = */
				/*m_nfluxmap = m_nfluxcol =*/ m_n_facet_x = m_n_facet_y = m_cant_type = m_focus_type = m_n_flux_days = m_delta_flux_hrs = -1;

			// Doubles
			m_helio_width = m_helio_height = m_helio_optical_error = m_helio_active_fraction = m_dens_mirror = m_helio_reflectance = m_rec_absorptance = m_rec_height = m_rec_aspect =
				m_rec_hl_perm2 = m_q_design = m_h_tower = m_land_max = m_land_min = m_p_start = m_p_track = m_hel_stow_deploy = m_v_wind_max = m_interp_nug =
				m_interp_beta = m_c_atm_0 = m_c_atm_1 = m_c_atm_2 = m_c_atm_3 = m_dni_des = m_land_area = std::numeric_limits<double>::quiet_NaN();

			// double *
			/*m_land_bound_table = m_land_bound_list = m_helio_positions = */ /*m_helio_aim_points =*/ /*m_eta_map =*/ /*m_flux_positions =*/ /*m_flux_maps =*/ /*NULL;*/

			// strings
			m_weather_file = "";
		}		
	};

	S_params ms_params;

	struct S_outputs
	{
		util::matrix_t<double> m_flux_map_out;
		double m_pparasi;
		double m_eta_field;

		S_outputs()
		{
			m_pparasi = m_eta_field = std::numeric_limits<double>::quiet_NaN();
		}
	};

	S_outputs ms_outputs;

	//void init(bool(*callback)(simulation_info* siminfo, void *data), void *cdata);
	void init();

	void call(const C_csp_weatherreader::S_outputs *p_weather, double field_control_in, const C_csp_solver_sim_info *p_sim_info);

	void converged();
};






#endif