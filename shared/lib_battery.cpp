#include <math.h>

#include "lib_battery.h"

/* 
Define Capacity Model 
*/

capacity_t::capacity_t(double q20, double I20, double V)
{
	_q20 = q20;
	_q0 = q20;
	_I20 = I20;
	_V = V;

	// Initialize SOC to 1, DOD to 0
	_SOC = 1;
	_DOD = 0;
}

bool capacity_t::chargeChanged()
{
	return _chargeChange;
}

double capacity_t::getDOD()
{
	return _DOD;
}

double capacity_t::get20HourCapacity()
{
	return _q20;
}
double capacity_t::getTotalCapacity()
{
	return _q0;
}
/*
Define KiBam Capacity Model
*/

capacity_kibam_t::capacity_kibam_t(double q20, double I20, double V, double t1, double t2, double q1, double q2) : 
capacity_t(q20, I20, V)
{
	// parameters for c, k calculation
	_q1 = q1;
	_q2 = q2;
	_t1 = t1;
	_t2 = t2;
	_F1 = q1 / q20; // use t1, 20
	_F2 = q1 / q2;  // use t1, t2

	// compute the parameters
	parameter_compute();

	// Initialize charge quantities.  
	// Assumes battery is initially fully charged
	_q1_0 = q20*_c;
	_q2_0 = _q0 - _q1_0;
	_chargeChange = false;
	_prev_charging = false;

	// Initialize other variables
	// Assumes initial current is 20 hour discharge current
	double T = _q0 / _I20;
	_qmaxI = qmax_of_i_compute(T);

	// Setup output structure
	_output = new output[TOTAL_CAPACITY_OUT];
	_output[TOTAL_CHARGE].name = "q0"; 
	_output[AVAILABLE_CHARGE].name = "q1"; 
	_output[BOUND_CHARGE].name = "q2"; 
	_output[POWER_DURING_STEP].name = "P";
	_output[STATE_OF_CHARGE].name = "SOC";
	_output[DEPTH_OF_DISCHARGE].name = "DOD"; 
	_output[MAX_CHARGE_AT_CURRENT].name = "qmaxI"; 
	_output[CURRENT].name = "I"; 

	_output[TOTAL_CHARGE].value = q20;
	_output[AVAILABLE_CHARGE].value = _q1_0;
	_output[BOUND_CHARGE].value = _q2_0;
	_output[POWER_DURING_STEP].value = 0;
	_output[STATE_OF_CHARGE].value = 1.0;
	_output[DEPTH_OF_DISCHARGE].value = 0.;
	_output[MAX_CHARGE_AT_CURRENT].value = _qmaxI;
	_output[CURRENT].value = I20;
}

capacity_kibam_t::~capacity_kibam_t()
{
	delete[] _output;
}

double capacity_kibam_t::c_compute(double F, double t1, double t2, double k_guess)
{
	double num = F*(1 - exp(-k_guess*t1))*t2 - (1 - exp(-k_guess*t2))*t1;
	double denom = F*(1 - exp(-k_guess*t1))*t2 - (1 - exp(-k_guess*t2))*t1 - k_guess*F*t1*t2 + k_guess*t1*t2;
	return (num / denom);
}

double capacity_kibam_t::q1_compute(double q10, double q0, double dt, double I)
{
	double A = q10*exp(-_k*dt);
	double B = (q0*_k*_c - I)*(1 - exp(-_k*dt)) / _k;
	double C = I*_c*(_k*dt - 1 + exp(-_k*dt)) / _k;
	return (A + B - C);
}

double capacity_kibam_t::q2_compute(double q20, double q0, double dt, double I)
{
	double A = q20*exp(-_k*dt);
	double B = q0*(1 - _c)*(1 - exp(-_k*dt));
	double C = I*(1 - _c)*(_k*dt - 1 + exp(-_k*dt)) / _k;
	return (A + B - C);
}

double capacity_kibam_t::Icmax_compute(double q10, double q0, double dt)
{
	double num = -_k*_c*_qmax + _k*q10*exp(-_k*dt) + q0*_k*_c*(1 - exp(-_k*dt));
	double denom = 1 - exp(-_k*dt) + _c*(_k*dt - 1 + exp(-_k*dt));
	return (num / denom);
}

double capacity_kibam_t::Idmax_compute(double q10, double q0, double dt)
{
	double num = _k*q10*exp(-_k*dt) + q0*_k*_c*(1 - exp(-_k*dt));
	double denom = 1 - exp(-_k*dt) + _c*(_k*dt - 1 + exp(-_k*dt));
	return (num / denom);
}

double capacity_kibam_t::qmax_compute()
{
	double num = _q20*((1 - exp(-_k * 20)) * (1 - _c) + _k*_c * 20);
	double denom = _k*_c * 20;
	return (num / denom);
}

double capacity_kibam_t::qmax_of_i_compute(double T)
{
	return ((_qmax*_k*_c*T) / (1 -exp(-_k*T) + _c*(_k*T - 1 + exp(-_k*T))));
}
void capacity_kibam_t::parameter_compute()
{
	double k_guess = 0.;
	double c1 = 0.;
	double c2 = 0.;
	double minRes = 10000.;

	for (int i = 0; i < 1000; i++)
	{
		k_guess = i*0.01;
		c1 = c_compute(_F1, _t1, 20, k_guess);
		c2 = c_compute(_F2, _t1, _t2, k_guess);

		if (fabs(c1 - c2) < minRes)
		{
			minRes = fabs(c1 - c2);
			_k = k_guess;
			_c = 0.5*(c1 + c2);
		}
	}
	_qmax = qmax_compute();
}

output* capacity_kibam_t::updateCapacity(double P, double V, double dt)
{
	double I = P / V;
	double Idmax = 0.;
	double Icmax = 0.;
	double Id = 0.;
	double Ic = 0.;
	double q1 = 0.;
	double q2 = 0.;
	bool charging = false;
	bool no_charge = false;

	if (I > 0)
	{
		Idmax = Idmax_compute(_q1_0, _q0, dt);
		Id = fmin(I, Idmax);
		I = Id;
	}
	else if (I < 0 )
	{
		Icmax = Icmax_compute(_q1_0, _q0, dt);
		Ic = -fmin(fabs(I), fabs(Icmax));
		I = Ic;
		charging = true;
	}
	else
	{
		no_charge = true;
	}

	// Check if charge changed
	if (charging != _prev_charging && !no_charge)
		_chargeChange = true;
	else
		_chargeChange = false;

	// new charge levels
	q1 = q1_compute(_q1_0, _q0, dt, I);
	q2 = q2_compute(_q2_0, _q0, dt, I);

	// update max charge at this current
	if (fabs(I) > 0)
		_qmaxI = qmax_of_i_compute(fabs(_qmaxI / I));
	else
	{
		//  just leave alone for timestep?
		_qmaxI = _qmax;
	}

	// update the SOC
	_SOC = (q1 + q2) / _qmaxI;
	_DOD = 1 - _SOC;

	// update internal variables for next time
	_q1_0 = q1;
	_q2_0 = q2;
	_q0 = q1 + q2;
	
	// return variables
	_output[TOTAL_CHARGE].value = _q0;
	_output[AVAILABLE_CHARGE].value = _q1_0;
	_output[BOUND_CHARGE].value = _q2_0;
	_output[POWER_DURING_STEP].value = P;
	_output[STATE_OF_CHARGE].value = _SOC;
	_output[DEPTH_OF_DISCHARGE].value = _DOD;
	_output[MAX_CHARGE_AT_CURRENT].value = _qmaxI;
	_output[CURRENT].value = I;

	return _output;
}

double capacity_kibam_t::getAvailableCapacity()
{
	return _q1_0;
}
double capacity_kibam_t::getMaxCapacityAtCurrent()
{
	return _qmaxI;
}

/*
Define Lifetime Model
*/

lifetime_t::lifetime_t( std::vector<double> DOD_vect, std::vector<double> cycle_vect, int n )
{
	_DOD_vect = new double[n];
	_cycle_vect = new double[n]; 

	for (int i = 0; i != n; i++)
	{
		_DOD_vect[i] = DOD_vect[i];
		_cycle_vect[i] = cycle_vect[i];
	}

	// Perform Curve fit
	_a = new double[n];
	for (int i = 0; i != n; i++)
		_a[i] = 0;
	int info = lsqfit(life_vs_DOD, 0, _a, n, _DOD_vect, _cycle_vect, n);
	int j;

	// initialize other member variables
	_nCycles = 0;
	_Dlt = 0;
	_jlt = 0;
	_klt = 0; 
	_Xlt = 0;
	_Ylt = 0;
	_Slt = 0;
	_Range = 0;

	// initialize output
	_output = new output[TOTAL_LIFETIME_OUT];
	_output[FRACTIONAL_DAMAGE].name = "Dlt";
	_output[NUMBER_OF_CYCLES].name = "nCycles";
}

lifetime_t::~lifetime_t()
{
	delete[] _DOD_vect;
	delete[] _cycle_vect;
	delete[] _a;
	delete[] _output;
}

output* lifetime_t::rainflow(double DOD)
{
	// initialize return code
	int retCode = LT_GET_DATA;

	// Begin algorithm
	_Peaks.push_back(DOD);
	bool atStepTwo = true;

	// Assign S, which is the starting peak or valley
	if (_jlt == 0)
	{
		_Slt = DOD;
		_klt = _jlt;
	}

	// Loop until break
	while (atStepTwo)
	{
		// Rainflow: Step 2: Form ranges X,Y
		if (_jlt >= 2)
			rainflow_ranges();
		else
		{
			// Get more data (Step 1)
			retCode = LT_GET_DATA;
			break;
		}

		// Rainflow: Step 3: Compare ranges
		retCode = rainflow_compareRanges();

		// We break to get more data, or if we are done with step 5
		if (retCode == LT_GET_DATA)
			break;
	}

	if (retCode == LT_GET_DATA)
		_jlt++;

	// Return output
	_output[FRACTIONAL_DAMAGE].value = _Dlt;
	_output[NUMBER_OF_CYCLES].value = _nCycles;

	return _output;
}

void lifetime_t::rainflow_ranges()
{
	_Ylt = fabs(_Peaks[_jlt - 1] - _Peaks[_jlt - 2]);
	_Xlt = fabs(_Peaks[_jlt] - _Peaks[_jlt - 1]);
}
void lifetime_t::rainflow_ranges_circular(int index)
{
	int end = _Peaks.size() - 1;
	if (index == 0)
	{
		_Xlt = fabs(_Peaks[0] - _Peaks[end]);
		_Ylt = fabs(_Peaks[end] - _Peaks[end - 1]);
	}
	else if (index == 1)
	{
		_Xlt = fabs(_Peaks[1] - _Peaks[0]);
		_Ylt = fabs(_Peaks[0] - _Peaks[end]);
	}
	else
		rainflow_ranges();
}

int lifetime_t::rainflow_compareRanges()
{
	int retCode = LT_SUCCESS;
	bool contained = true;

	if (_Xlt < _Ylt)
		retCode = LT_GET_DATA;
	else if (_Xlt == _Ylt)
	{
		if ((_Slt == _Peaks[_jlt - 1]) || (_Slt == _Peaks[_jlt - 2]))
			retCode = LT_GET_DATA;
		else
			contained = false;
	}
	else if (_Xlt >= _Ylt)
	{
		if (_Xlt > _Ylt)
		{
			if ((_Slt == _Peaks[_jlt - 1]) || (_Slt == _Peaks[_jlt - 2]))
			{
				// Step 4: Move S to next point in vector, then go to step 1
				_klt++;
				_Slt = _Peaks[_klt];
				retCode = LT_GET_DATA;
			}
			else
				contained = false;
		}
		else if (_Xlt == _Ylt)
		{
			if ((_Slt != _Peaks[_jlt - 1]) && (_Slt != _Peaks[_jlt - 2]))
				contained = false;
		}

	}

	// Step 5: Count range Y, discard peak & valley of Y, go to Step 2
	if (!contained)
	{
		_Range = _Ylt;
		double Cf = life_vs_DOD(_Range, _a, 0);
		_Dlt += 1. / Cf;
		_nCycles++;
		// discard peak & valley of Y
		double save = _Peaks[_jlt];
		_Peaks.pop_back(); 
		_Peaks.pop_back();
		_Peaks.pop_back();
		_Peaks.push_back(save);
		_jlt -= 2;
		// stay in while loop
		retCode = LT_RERANGE;
	}

	return retCode;
}

output* lifetime_t::rainflow_finish()
{
	// starting indices, must decrement _jlt by 1
	int ii = 0;
	_jlt--;
	double P = 0.;
	int rereadCount = 0;


	while ( rereadCount <= 1 )
	{
		if (ii < _Peaks.size())
			P = _Peaks[ii];
		else
			break;

		// Step 6
		if (P == _Slt)
			rereadCount++;

		bool atStepSeven = true;

		// Step 7: Form ranges X,Y
		while (atStepSeven)
		{
			if (_jlt >= 2)
				rainflow_ranges_circular(ii);
			else
			{
				atStepSeven = false;
				if (_jlt == 1)
				{
					_Peaks.push_back(P);
					_jlt++;
					// move to end point
					ii = _jlt;
					rainflow_ranges_circular(ii);
				}
				else
					break;
			}

			// Step 8: compare X,Y
			if (_Xlt < _Ylt)
			{
				atStepSeven = false;
				// move to next point (Step 6)
				ii++;
			}
			else
			{
				_Range = _Ylt;
				double Cf = life_vs_DOD(_Range, _a, 0);
				_Dlt += 1. / Cf;
				_nCycles++;
				// Discard peak and vally of Y
				double save = _Peaks[_jlt];
				_Peaks.pop_back();
				_Peaks.pop_back();
				_Peaks.pop_back();
				_Peaks.push_back(save);
				_jlt -= 2;
			}
		}
	}
	// Return output
	_output[FRACTIONAL_DAMAGE].value = _Dlt;
	_output[NUMBER_OF_CYCLES].value = _nCycles;

	return _output;
}

double life_vs_DOD(double R, double * a, void * user_data)
{
	// may need to figure out how to make more robust (if user enters only three pairs)
	return (a[0] + a[1] * exp(a[2] * R) + a[3] * exp(a[4] * R));
}

/* 
Define Battery 
*/
battery_t::battery_t(capacity_t *capacity, lifetime_t * lifetime, double dt)
{
	_capacity = capacity;
	_lifetime = lifetime;
	_dt = dt;
	_firstStep = true;
}
void battery_t::run(double P, double V)
{
	double lastDOD = _capacity->getDOD();

	if (lastDOD <= 1. && lastDOD >= 0.)
	{
		if (_capacity->chargeChanged() || _firstStep)
		{
			_LifetimeOutput = runLifetimeModel(lastDOD);
			_firstStep = false;
		}
	}
	_CapacityOutput = runCapacityModel(P, V);
}

void battery_t::finish()
{
	_LifetimeOutput = _lifetime->rainflow_finish();
}

output* battery_t::runCapacityModel(double P, double V)
{
	return _capacity->updateCapacity(P, V, _dt);
}

output* battery_t::runLifetimeModel(double DOD)
{
	return _lifetime->rainflow(DOD);
}

output* battery_t::getCapacityOutput()
{
	return _CapacityOutput;
}

output* battery_t::getLifetimeOutput()
{
	return _LifetimeOutput;
}

double battery_t::chargeNeededToFill()
{
	double charge_needed =_capacity->getMaxCapacityAtCurrent() - _capacity->getTotalCapacity();
	if (charge_needed > 0)
		return charge_needed;
	else
		return 0.;
}

double battery_t::getCurrentCharge()
{
	return _capacity->getAvailableCapacity();
}


/*
Non-class function
*/
void getMonthHour(int hourOfYear, int * out_month, int * out_hour)
{
	int tmpSum = 0;
	int hour = 0;
	int month;

	for ( month = 1; month <= 12; month++)
	{
		int hoursInMonth = util::hours_in_month(month);
		tmpSum += hoursInMonth;

		// found the month
		if (hourOfYear + 1 <= tmpSum)
		{
			// get the day of the month
			int tmp = floor((float)(hourOfYear) / 24);
			hour = (hourOfYear + 1) - (tmp * 24);
			break;
		}
	}

	*out_month = month;
	*out_hour = hour;

}