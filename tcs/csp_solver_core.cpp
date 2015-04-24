#include "csp_solver_core.h"
#include "csp_solver_util.h"

#include "lib_util.h"

C_csp_solver::C_csp_solver(C_csp_weatherreader &weather,
	C_csp_collector_receiver &collector_receiver,
	C_csp_power_cycle &power_cycle) : 
	mc_weather(weather), 
	mc_collector_receiver(collector_receiver), 
	mc_power_cycle(power_cycle)
{
	// Inititalize non-reference member data
	m_T_htf_cold_des = m_cycle_W_dot_des = m_cycle_eta_des = m_cycle_q_dot_des = m_cycle_max_frac = m_cycle_cutoff_frac =
		m_cycle_sb_frac_des = m_cycle_T_htf_hot_des = std::numeric_limits<double>::quiet_NaN();

	m_op_mode_tracking.resize(0);
}

void C_csp_solver::init_independent()
{
	mc_weather.init();
	mc_collector_receiver.init();
	mc_power_cycle.init();

	return;
}

void C_csp_solver::init()
{
	init_independent();

	// Get controller values from component models
		// Collector/Receiver
	C_csp_collector_receiver::S_csp_cr_solved_params cr_solved_params;
	mc_collector_receiver.get_design_parameters(cr_solved_params);
	m_T_htf_cold_des = cr_solved_params.m_T_htf_cold_des;	//[K]

		// Power Cycle
	C_csp_power_cycle::S_solved_params solved_params;
	mc_power_cycle.get_design_parameters(solved_params);		
	m_cycle_W_dot_des = solved_params.m_W_dot_des;					//[MW]
	m_cycle_eta_des = solved_params.m_eta_des;						//[-]
	m_cycle_q_dot_des = solved_params.m_q_dot_des;					//[MW]
	m_cycle_max_frac = solved_params.m_max_frac;				//[-]
	m_cycle_cutoff_frac = solved_params.m_cutoff_frac;		//[-]
	m_cycle_sb_frac_des = solved_params.m_sb_frac;			//[-]
	m_cycle_T_htf_hot_des = solved_params.m_T_htf_hot_ref+273.15;	//[K] convert from C
}


void C_csp_solver::simulate()
{
	
	double sim_time_start = 0.0;			//[s] hardcode simulation to start at first of year, for now
	double sim_time_end = 8760.0*3600;		//[s] hardcode simulation to run through entire year, for now
	double sim_step_size_baseline = 3600.0;			//[s]
	mc_sim_info.m_step = sim_step_size_baseline;		//[s] hardcode steps = 1 hr, for now

	C_csp_solver_htf_state cr_htf_state;
	C_csp_collector_receiver::S_csp_cr_inputs cr_inputs;
	C_csp_collector_receiver::S_csp_cr_outputs cr_outputs;

	C_csp_solver_htf_state pc_htf_state;
	C_csp_power_cycle::S_control_inputs pc_inputs;
	C_csp_power_cycle::S_csp_pc_outputs pc_outputs;

	bool is_rec_su_allowed = true;
	bool is_pc_su_allowed = true;
	bool is_pc_sb_allowed = true;

	int cr_operating_state = C_csp_collector_receiver::E_csp_cr_modes::OFF;
	int pc_operating_state = C_csp_power_cycle::E_csp_power_cycle_modes::OFF;

	bool is_est_rec_output_useful = false;

	double tol_mode_switching = 0.05;		// Give buffer to account for uncertainty in estimates

	double step_local = mc_sim_info.m_step;		//[hr] Step size might adjust during receiver and/or power cycle startup
	bool is_sim_timestep_complete = true;		//[-] Are we running serial simulations at partial timesteps inside of one typical timestep?

	double time_previous = sim_time_start;		//[s]

	double time_sim_step_next = sim_time_start + sim_step_size_baseline;	//[s]

	mc_sim_info.m_step = step_local;						//[s]
	mc_sim_info.m_time = time_previous + step_local;		//[s]

	m_op_mode_tracking.resize(0);

	while( mc_sim_info.m_time <= sim_time_end )
	{
		// Get collector/receiver & power cycle operating states
		cr_operating_state = mc_collector_receiver.get_operating_state();
		pc_operating_state = mc_power_cycle.get_operating_state();

		// Get weather at this timestep. Should only be called once per timestep. (Except converged() function)
		mc_weather.timestep_call(mc_sim_info);

		// Get or set decision variables
		bool is_rec_su_allowed = true;
		bool is_pc_su_allowed = true;
		bool is_pc_sb_allowed = true;
		int tou_timestep = 1;			//[base 1] used by power cycle model for hybrid cooling - may also want to move this to controller

		// Get standby fraction and min operating fraction
			// Could eventually be a method in PC class...
		double cycle_sb_frac = m_cycle_sb_frac_des;				//[MW]
			
			// *** If standby not allowed, then reset q_pc_sb = q_pc_min ?? *** or is this too confusing and not helpful enough?
		double q_pc_sb = cycle_sb_frac * m_cycle_q_dot_des;		//[MW]
		double q_pc_min = m_cycle_cutoff_frac * m_cycle_q_dot_des;	//[MW]
		double q_pc_max = m_cycle_max_frac * m_cycle_q_dot_des;		//[MW]

		// Solve collector/receiver with design inputs and weather to estimate output
			// May replace this call with a simple proxy model later...
		cr_htf_state.m_temp_in = m_T_htf_cold_des - 273.15;		//[C], convert from [K]
		cr_inputs.m_field_control = 1.0;						//[-] no defocusing for initial simulation
		cr_inputs.m_input_operation_mode = C_csp_collector_receiver::E_csp_cr_modes::STEADY_STATE;
		mc_collector_receiver.call(mc_weather.ms_outputs,
			cr_htf_state,
			cr_inputs,
			cr_outputs,
			mc_sim_info);

		double q_dot_cr_output = cr_outputs.m_q_thermal;		//[MW]

		// Can receiver output be used?
			// No TES
		is_est_rec_output_useful = q_dot_cr_output*(1.0+tol_mode_switching) > q_pc_sb;

		
		int operating_mode = -1;
		bool are_models_converged = false;
		
		// Determine which operating mode to try first
		if( is_est_rec_output_useful )		// Can receiver produce power and can it be used somewhere (power cycle, in this case)
		{
			if( (cr_operating_state==C_csp_collector_receiver::OFF || cr_operating_state==C_csp_collector_receiver::STARTUP) 
				&& pc_operating_state==C_csp_power_cycle::OFF)
			{	// At start of this timestep, are power cycle AND collector/receiver off?
				
				if(is_rec_su_allowed && is_pc_su_allowed)
				{	// Are receiver and power cycle startup allowed?
					
					if( q_dot_cr_output*(1.0 + tol_mode_switching) > q_pc_min )
					{	// Do we estimate that there is enough thermal power to operate cycle at min fraction?

						operating_mode = CR_SU__PC_OFF__TES_OFF__AUX_OFF;
					}
					else if( is_pc_sb_allowed && q_dot_cr_output*(1.0 + tol_mode_switching) > q_pc_sb )
					{	// If not, is there enough to operate at standby, AND is standby allowed

						operating_mode = CR_SU__PC_OFF__TES_OFF__AUX_OFF;
					}
					else
					{	// If we can't use the receiver output, then don't start up

						operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
					}				
				}
				else
				{	// If startup isn't allowed, then don't try

					operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
				}
			}
			else if(cr_operating_state==C_csp_collector_receiver::ON && 
					(pc_operating_state==C_csp_power_cycle::OFF || pc_operating_state==C_csp_power_cycle::STARTUP) )
			{	// At start of this timestep, is collector/receiver on, but the power cycle is off?
				
				if( is_pc_su_allowed )
				{	// Is power cycle startup allowed?
					
					if( q_dot_cr_output*(1.0 + tol_mode_switching) > q_pc_min )
					{
						operating_mode = CR_ON__PC_SU__TES_OFF__AUX_OFF;
					}
					else if( is_pc_sb_allowed && q_dot_cr_output*(1.0 + tol_mode_switching) > q_pc_sb )
					{
						operating_mode = CR_ON__PC_SU__TES_OFF__AUX_OFF;
					}
					else
					{
						operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
					}					
				}
				else
				{	// If startup isn't allowed, then don't try

					operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
				}				
			}
			else if(cr_operating_state==C_csp_collector_receiver::ON &&
					pc_operating_state==C_csp_power_cycle::ON)
			{	// Are the collector/receiver AND power cylce ON?

				if( q_dot_cr_output*(1.0-tol_mode_switching) > q_pc_max )
				{	// Is it likely that the receiver will defocus?
					// If this mode is entered as the initial mode, then controller can't go back to CR_ON__PC_RM__TES_OFF__AUX_OFF
					
					operating_mode = CR_DF__PC_FULL__TES_OFF__AUX_OFF;

					throw(C_csp_exception("Defocus mode not yet available", "CSP Solver"));
				}
				else if( q_dot_cr_output*(1.0 + tol_mode_switching) > q_pc_min )
				{	// Receiver can likely operate at full available power

					operating_mode = CR_ON__PC_RM__TES_OFF__AUX_OFF;
				}
				else if(is_pc_sb_allowed && q_dot_cr_output*(1.0 + tol_mode_switching) > q_pc_sb ) // this is the entry logic to the outer nest 
				{	// Receiver can likely operate in standby
				
					operating_mode = CR_ON__PC_SB__TES_OFF__AUX_OFF;
				}
				else
				{	// Can't use receiver output, so shutdown CR and PC
					// Shouldn't end up here because of outer-nest entry logic, but include as a safety check 

					operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
				}
			}
		}
		else
		{
			operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
		}


		while(!are_models_converged)		// Solve for correct operating mode and performance in following loop:
		{
			switch(operating_mode)
			{
			
			case CR_ON__PC_RM__TES_OFF__AUX_OFF:
			{
				// Collector/Receiver in ON, and only place for HTF to go is power cycle.
				// Therefore, power cycle must operate at Resource Match and use w/e is provided
					// (in cases with storage or field defocus, power cycle will try to hit an exact thermal input)
				// 'Failure Modes'
				// 1) Receiver provides too much power
				//		* Go to defocus
				// 2) Receiver cannot maintain minimum operation fraction
				//		* Go to power cycle standby or shutdown

				
				// Store operating mode
				m_op_mode_tracking.push_back(operating_mode);

				// Solution procedure
				// 1) Guess the receiver inlet temperature
						// Use design temperature for now, but this is an area where "smart" guesses could be applied
				double T_rec_in_guess_ini = m_T_htf_cold_des - 273.15;		//[C], convert from [K]
				double T_rec_in_guess = T_rec_in_guess_ini;
						// Set lower and upper bounds, or find through iteration?
						// Lower bound could be freeze protection temperature...
				double T_rec_in_lower = std::numeric_limits<double>::quiet_NaN();
				double T_rec_in_upper = std::numeric_limits<double>::quiet_NaN();
				double y_rec_in_lower = std::numeric_limits<double>::quiet_NaN();
				double y_rec_in_upper = std::numeric_limits<double>::quiet_NaN();
						// Booleans for bounds and convergence error
				bool is_upper_bound = false;
				bool is_lower_bound = false;
				bool is_upper_error = false;
				bool is_lower_error = false;

				double tol_C = 2.0;
				double tol = tol_C / m_T_htf_cold_des;

				double safety_tol_multiplier = 5.0;
				double safety_tol = safety_tol_multiplier*tol;

				double diff_T_in = 999.9*tol;		// (Calc - Guess)/Guess: (+) Guess was too low, (-) Guess was too high

				int iter_T_in = 0;

				// If convergence/iteration loop breaks without solution, then 'are_models_converged' MUST BE RESET = FALSE before break
					// otherwise, loop exits to TRUE
				are_models_converged = true;
				

				while( abs(diff_T_in) > tol )
				{
					iter_T_in++;			// First iteration = 1

					// Check if distance between bounds is "too small"
					double diff_T_bounds = T_rec_in_upper - T_rec_in_lower;
					if(diff_T_bounds/T_rec_in_upper < tol/2.0)
					{
						if(diff_T_in != diff_T_in)
						{	// Models aren't producing power or are returning errors, and it appears we've tried the solution space for T_rec_in
							// Shut down receiver and power cycle
							operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
							are_models_converged = false;
							break;	// Gets out of while() loop?							
						}
						else if(abs(diff_T_in) < safety_tol)
						{	// Models are producing power, but convergence errors are not within Safety Tolerance. Shut down receiver and power cycle
							// Shut down receiver and power cycle
							operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
							are_models_converged = false;
							break;	// Gets out of while() loop?										
						}
						else
						{	// Getting results and convergence errors are within Safety Tolerance. *Report message* and continue timeseries simulation with receiver ON
							
							error_msg = util::format("At time = %lg the collector/receiver and power cycle solution only reached a converge"
								"= %lg. Check that results at this timestep are not unreasonably biasing total simulation results", mc_sim_info.m_time / 3600.0, diff_T_in);
							mc_csp_messages.add_message(C_csp_messages::WARNING, error_msg);

							are_models_converged = true;
							break;
						}					
					}


					// Subsequent iterations need to re-calcualte T_in
					if(iter_T_in > 1)
					{
						if(diff_T_in != diff_T_in)
						{	// Models did not solve such that a convergence error could be generated
							// However, we know that upper and lower bounds are set, so we can calculate a new guess via bisection method
								// but check that bounds exist, to be careful
							if(!is_lower_bound || !is_upper_bound)
							{	 
								// Shut down receiver and power cycle
								operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
								are_models_converged = false;
								break;	// Gets out of while() loop?
							}
							T_rec_in_guess = 0.5*(T_rec_in_lower + T_rec_in_upper);		//[C]
						}
						else if(diff_T_in > 0.0)		// Guess receiver inlet temperature was too low
						{
							is_lower_bound = true;
							is_lower_error = true;
							T_rec_in_lower = T_rec_in_guess;		// Set lower bound
							y_rec_in_lower = diff_T_in;				// Set lower convergence error

							if(is_upper_bound && is_upper_error)		// False-position method
							{
								T_rec_in_guess = y_rec_in_upper/(y_rec_in_upper-y_rec_in_lower)*(T_rec_in_lower-T_rec_in_upper)+T_rec_in_upper;	//[C]
							}
							else if(is_upper_bound)						// Bisection method
							{
								T_rec_in_guess = 0.5*(T_rec_in_lower + T_rec_in_upper);		//[C]
							}
							else				// Constant adjustment
							{
								T_rec_in_guess += 15.0;			//[C]
							}
						}
						else							// Guess receiver inlet temperature was too high
						{
							is_upper_bound = true;
							is_upper_error = true;
							T_rec_in_upper = T_rec_in_guess;		// Set upper bound
							y_rec_in_upper = diff_T_in;				// Set upper convergence error

							if(is_lower_bound && is_lower_error)		// False-position method
							{
								T_rec_in_guess = y_rec_in_upper / (y_rec_in_upper - y_rec_in_lower)*(T_rec_in_lower - T_rec_in_upper) + T_rec_in_upper;	//[C]
							}
							else if(is_lower_bound)
							{
								T_rec_in_guess = 0.5*(T_rec_in_lower + T_rec_in_upper);		//[C]
							}
							else
							{
								T_rec_in_guess -= 15.0;			//[C] 
							}
						}
					}

					// 2) Solve the receiver model

					// CR: ON
					cr_htf_state.m_temp_in = m_T_htf_cold_des - 273.15;		//[C], convert from [K]
					cr_inputs.m_field_control = 1.0;						//[-] no defocusing for initial simulation
					cr_inputs.m_input_operation_mode = C_csp_collector_receiver::ON;

					mc_collector_receiver.call(mc_weather.ms_outputs,
						cr_htf_state,
						cr_inputs,
						cr_outputs,
						mc_sim_info);

					// Check if receiver is OFF or model didn't solve
					if( cr_outputs.m_m_dot_salt_tot == 0.0 || cr_outputs.m_q_thermal == 0.0 )
					{
						// If first iteration, don't know enough about why collector/receiver is not producing power to advance iteration
						// Go to Receiver OFF power cycle OFF
						if( iter_T_in == 1 )
						{	// Shut down receiver and power cycle
							operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
							are_models_converged = false;
							break;	// Gets out of while() loop?
						}
						else
						{	// Set this T_rec_in_guess as either upper or lower bound, depending on which end of DESIGN temp it falls
							// Assumption here is that receiver solved at first guess temperature
							// and that the failure wouldn't occur between established bounds
							if( T_rec_in_guess < T_rec_in_guess_ini )
							{
								if(is_lower_bound && !is_upper_bound)
								{
									// Shut down receiver and power cycle
									operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
									are_models_converged = false;
									break;	// Gets out of while() loop?
								}
								T_rec_in_lower = T_rec_in_guess;
								is_lower_bound = true;
								is_lower_error = false;
								// At this point, both and upper and lower bound should exist, so can generate new guess
								// And communicate this to Guess-Generator by setting diff_T_in to NaN
								diff_T_in = std::numeric_limits<double>::quiet_NaN();
							}
							else
							{
								if(is_upper_bound && !is_lower_bound)
								{
									// Shut down receiver and power cycle
									operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
									are_models_converged = false;
									break;	// Gets out of while() loop?
								}
								T_rec_in_upper = T_rec_in_guess;
								is_upper_bound = true;
								is_upper_error = false;
								// At this point, both and upper and lower bound should exist, so can generate new guess
								// And communicate this to Guess-Generator by setting diff_T_in to NaN
								diff_T_in = std::numeric_limits<double>::quiet_NaN();
							}
						}
					}	// End Collector/Receiver OFF decisions

					// 3) Solve the power cycle model using receiver outputs
					// Power Cycle: ON
					pc_htf_state.m_temp_in = cr_outputs.m_T_salt_hot;		//[C]
					pc_htf_state.m_m_dot = cr_outputs.m_m_dot_salt_tot;		//[kg/hr] no mass flow rate to power cycle
					// Inputs
					pc_inputs.m_standby_control = C_csp_power_cycle::E_csp_power_cycle_modes::ON;
					pc_inputs.m_tou = tou_timestep;
					// Performance Call
					mc_power_cycle.call(mc_weather.ms_outputs,
						pc_htf_state,
						pc_inputs,
						pc_outputs,
						mc_sim_info);

					// Check that power cycle is producing power or model didn't solve
					if( pc_outputs.m_P_cycle == 0.0 )
					{
						// If first iteration, don't know enough about why power cycle is not producing power to advance iteration
						// Go to Receiver OFF power cycle OFF
						if( iter_T_in == 1 )
						{
							// Shut down receiver and power cycle
							operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
							are_models_converged = false;
							break;	// Gets out of while() loop?
						}
						else
						{	// Set this T_rec_in_guess as either upper or lower bound, depending on which end of DESIGN temp it falls
							// Assumption here is that receiver solved at first guess temperature
							// and that the failure wouldn't occur between established bounds
							if( T_rec_in_guess < T_rec_in_guess_ini )
							{
								if(is_lower_bound && !is_upper_bound)
								{
									// Shut down receiver and power cycle
									operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
									are_models_converged = false;
									break;	// Gets out of while() loop?
								}
								T_rec_in_lower = T_rec_in_guess;
								is_lower_bound = true;
								is_lower_error = false;
								// At this point, both and upper and lower bound should exist, so can generate new guess
								// And communicate this to Guess-Generator by setting diff_T_in to NaN
								diff_T_in = std::numeric_limits<double>::quiet_NaN();
							}
							else
							{
								if(is_upper_bound && !is_lower_bound)
								{
									// Shut down receiver and power cycle
									operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
									are_models_converged = false;
									break;	// Gets out of while() loop?
								}
								T_rec_in_upper = T_rec_in_guess;
								is_upper_bound = true;
								is_upper_error = false;
								// At this point, both and upper and lower bound should exist, so can generate new guess
								// And communicate this to Guess-Generator by setting diff_T_in to NaN
								diff_T_in = std::numeric_limits<double>::quiet_NaN();
							}
						}
					}	// end Power Cycle OFF decisions

					diff_T_in = (pc_outputs.m_T_htf_cold - T_rec_in_guess) / T_rec_in_guess;

				}	// end iteration on T_rec_in

				// Now, check whether we need to defocus the receiver
				if( cr_outputs.m_q_thermal > q_pc_max )
				{	// Too much power to PC, try defocusing
					operating_mode = CR_DF__PC_FULL__TES_OFF__AUX_OFF;

					are_models_converged = false;
				}
				else if( cr_outputs.m_q_thermal < q_pc_min )
				{	// Not enough thermal power to run power cycle at Min Cutoff fraction: check if we can try standby
					
					if( is_pc_sb_allowed )
					{	// If controller *was* trying to generate power, then assume that there is enough power to at least try standby
					
						operating_mode = CR_ON__PC_SB__TES_OFF__AUX_OFF;
					}
					else
					{	// PC standby not allowed - shut down CR and PC
					
						operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
					}

					are_models_converged = false;
				}
				else
				{	// Solved successfully within bounds of this operation mode: move on

					are_models_converged = true;
				}


			}	// end case{} to allow compilation with local (w/r/t case) variables

				break;
			
			
			case CR_ON__PC_SB__TES_OFF__AUX_OFF:
				// Collector/receiver is ON
				// Power cycle is running in standby
				// During standby, assume power cycle HTF return temperature is constant and = m_T_htf_cold_des
					// so shouldn't need to iterate between CR and PC
				// Assume power cycle can remain in standby the entirety of the timestep

				
				// Store operating mode
				m_op_mode_tracking.push_back(operating_mode);

				// First, solve the CR. Again, we're assuming HTF inlet temperature is always = m_T_htf_cold_des
				cr_htf_state.m_temp_in = m_T_htf_cold_des - 273.15;		//[C], convert from [K]
				cr_inputs.m_field_control = 1.0;						//[-] no defocusing for initial simulation
				cr_inputs.m_input_operation_mode = C_csp_collector_receiver::ON;

				mc_collector_receiver.call(mc_weather.ms_outputs,
					cr_htf_state,
					cr_inputs,
					cr_outputs,
					mc_sim_info);

				if( cr_outputs.m_q_thermal == 0.0 )
				{	// Collector/receiver can't produce useful energy
					operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
					are_models_converged = false;
				}

				// If receiver is indeed producing power, then try power cycle at standby
					// Power cycle: STANDBY


				break;


			case CR_ON__PC_SU__TES_OFF__AUX_OFF:
				// Collector/receiver is ON
				// Startup power cycle
				// During startup, assume power cycle HTF return temperature is constant and = m_T_htf_cold_des
					// so shouldn't need to iterate between collector/receiver and power cycle
				// This will probably result in a local timestep shorter than the baseline simulation timestep (governed by weather file)


				// Store operating mode
				m_op_mode_tracking.push_back(operating_mode);

				// CR: ON
				cr_htf_state.m_temp_in = m_T_htf_cold_des - 273.15;		//[C], convert from [K]
				cr_inputs.m_field_control = 1.0;						//[-] no defocusing for initial simulation
				cr_inputs.m_input_operation_mode = C_csp_collector_receiver::ON;

				mc_collector_receiver.call(mc_weather.ms_outputs,
					cr_htf_state,
					cr_inputs,
					cr_outputs,
					mc_sim_info);

				if(cr_outputs.m_q_thermal == 0.0)
				{	// Collector/receiver can't produce useful energy
					operating_mode = CR_OFF__PC_OFF__TES_OFF__AUX_OFF;
					are_models_converged = false;
				}

				// If receiver IS producing energy, try starting up power cycle
					// Power Cycle: STARTUP
				pc_htf_state.m_temp_in = cr_outputs.m_T_salt_hot;		//[C]
				pc_htf_state.m_m_dot = cr_outputs.m_m_dot_salt_tot;		//[kg/hr] no mass flow rate to power cycle
				// Inputs
				pc_inputs.m_standby_control = C_csp_power_cycle::E_csp_power_cycle_modes::STARTUP;
				pc_inputs.m_tou = tou_timestep;
				// Performance Call
				mc_power_cycle.call(mc_weather.ms_outputs,
					pc_htf_state,
					pc_inputs,
					pc_outputs,
					mc_sim_info);

				// Would be nice to have some check to know whether startup solved appropriately...


				// Check for new timestep
				step_local = pc_outputs.m_time_required_su;		//[s] power cycle model returns MIN(time required to completely startup, full timestep duration)
				if( step_local < mc_sim_info.m_step )
				{
					is_sim_timestep_complete = false;
				}

				// Reset sim_info values
				if( !is_sim_timestep_complete )
				{
					mc_sim_info.m_step = step_local;						//[s]
					mc_sim_info.m_time = time_previous + step_local;		//[s]
				}


				are_models_converged = true;

				break;
			
			case CR_SU__PC_OFF__TES_OFF__AUX_OFF:
				// Run the collector/receiver under startup mode
					// **************
				// This will probably result in a local timestep shorter than the baseline simulation timestep (governed by weather file)

				// Store operating mode
				m_op_mode_tracking.push_back(operating_mode);

				cr_htf_state.m_temp_in = m_T_htf_cold_des - 273.15;		//[C], convert from [K]
				cr_inputs.m_field_control = 1.0;						//[-] no defocusing for initial simulation
				cr_inputs.m_input_operation_mode = C_csp_collector_receiver::STARTUP;

				mc_collector_receiver.call(mc_weather.ms_outputs,
					cr_htf_state,
					cr_inputs,
					cr_outputs,
					mc_sim_info);

				// Check for new timestep
				step_local = cr_outputs.m_time_required_su;		//[s] Receiver model returns MIN(time required to completely startup, full timestep duration)
				if(step_local < mc_sim_info.m_step)
				{
					is_sim_timestep_complete = false;
				}

				// Reset sim_info values
				if( !is_sim_timestep_complete )
				{
					mc_sim_info.m_step = step_local;						//[s]
					mc_sim_info.m_time = time_previous + step_local;		//[s]
				}

				// Power Cycle: OFF
				pc_htf_state.m_temp_in = m_cycle_T_htf_hot_des - 273.15;	//[C]
				pc_htf_state.m_m_dot = 0.0;		//[kg/hr] no mass flow rate to power cycle
				// Inputs
				pc_inputs.m_standby_control = C_csp_power_cycle::E_csp_power_cycle_modes::OFF;
				pc_inputs.m_tou = tou_timestep;
				// Performance Call
				mc_power_cycle.call(mc_weather.ms_outputs,
					pc_htf_state,
					pc_inputs,
					pc_outputs,
					mc_sim_info);

				are_models_converged = true;

				break;

			case tech_operating_modes::CR_OFF__PC_OFF__TES_OFF__AUX_OFF:
				// Solve all models as 'off' or 'idle'
					// Collector/receiver

				// Store operating mode
				m_op_mode_tracking.push_back(operating_mode);

				cr_htf_state.m_temp_in = m_T_htf_cold_des - 273.15;		//[C], convert from [K]
				cr_inputs.m_field_control = 1.0;						//[-] no defocusing for initial simulation
				cr_inputs.m_input_operation_mode = C_csp_collector_receiver::E_csp_cr_modes::OFF;
				mc_collector_receiver.call(mc_weather.ms_outputs,
					cr_htf_state,
					cr_inputs,
					cr_outputs,
					mc_sim_info);
					
					// Power Cycle: OFF
						// HTF State
				pc_htf_state.m_temp_in = m_cycle_T_htf_hot_des-273.15;	//[C]
				pc_htf_state.m_m_dot = 0.0;		//[kg/hr] no mass flow rate to power cycle
						// Inputs
				pc_inputs.m_standby_control = C_csp_power_cycle::E_csp_power_cycle_modes::OFF;
				pc_inputs.m_tou = tou_timestep;
						// Performance Call
				mc_power_cycle.call(mc_weather.ms_outputs,
					pc_htf_state,
					pc_inputs,
					pc_outputs,
					mc_sim_info);

				are_models_converged = true;
			
				break;		// exit switch() after CR_OFF__PC_OFF__TES_OFF__AUX_OFF:

			default: 
				double catch_here_for_now = 1.23;

			}	// End switch() on receiver operating modes
		
		}	// End loop to find correct operating mode and system performance


		// Timestep solved: run post-processing, converged()		
		mc_collector_receiver.converged();
		mc_power_cycle.converged();

				
		// Don't converge weather file if working with partial timesteps
		if( !is_sim_timestep_complete )
		{
			// Calculate new timestep
			step_local = time_sim_step_next - mc_sim_info.m_time;
		}
		else
		{
			// If partial timestep, use constant weather data for all partial timesteps
			mc_weather.converged();

			step_local = sim_step_size_baseline;

			time_sim_step_next += sim_step_size_baseline;
		}

		// Track time and step forward
		is_sim_timestep_complete = true;
		time_previous = mc_sim_info.m_time;						//[s]
		mc_sim_info.m_step = step_local;						//[s]
		mc_sim_info.m_time = time_previous + step_local;		//[s]
					
		// Reset operating mode tracker, so get "save" or write or pass results somewhere
		m_op_mode_tracking.resize(0);

	}	// End timestep loop

}	// End simulate() method