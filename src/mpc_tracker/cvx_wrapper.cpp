/* author: Daniel Hert */

extern "C" {
#include "cvxgen/solver.h"
}
#include <eigen3/Eigen/Eigen>
#include "cvx_wrapper.h"

using namespace Eigen;

Vars      vars;
Params    params;
Workspace work;
Settings  settings;

/* dim is used to offset the result in the output vecor, according to which dimension (x,y,z) is being calculated */
/* x - 0 */
/* y - 1 */
/* z - 2 */
/* yaw - 0 */

CvxWrapper::CvxWrapper(bool verbose, int max_iters, std::vector<double> tempQ, std::vector<double> tempR, double dt, double dt2, int dimension) {

  set_defaults();
  setup_indexing();
  setup_indexed_params();
  dim = dimension * 3;
  if (dim > 6 || dim < 0) {
    ROS_ERROR("CvxWrapper - parameter dim should be 0, 1 or 2 !!! setting to 0");
    dim = 0;
  }

  if ((verbose != 1 && verbose != 0) || !std::isfinite(verbose)) {
    ROS_ERROR("CvxWrapper - verbose has to be 0 or 1!!! Safe value of 0 set instead");
    verbose = 0;
  }
  settings.verbose = verbose;

  if ((max_iters < 1 || max_iters > 100) || !std::isfinite(max_iters)) {
    ROS_ERROR("CvxWrapper - max_iters wrong value!!! Safe value of 20 set instead");
    max_iters = 20;
  }
  settings.max_iters = max_iters;


  /* settings.eps       = 0.00001; */
  /* settings.resid_tol = 0.001; */

  if (tempQ.size() == 3) {
    for (int i = 0; i < 3; i++) {
      if (tempQ[i] >= 0 && std::isfinite(tempQ[i])) {
        params.Q[i] = tempQ[i];
      } else {
        ROS_ERROR_STREAM("CvxWrapper - Q matrix has to be PSD - parameter " << i << " !!! Safe value of 500 set instead");
        params.Q[i] = 500;
      }
    }
  } else {
    ROS_ERROR_STREAM("CvxWrapper - Q matrix wrong size " << tempQ.size() << " !!! Safe values set instead");
    params.Q[0] = 5000;
    params.Q[1] = 0;
    params.Q[2] = 0;
  }

  if (tempR.size() == 1) {
    for (int i = 0; i < 1; i++) {
      if (tempR[i] >= 0 && std::isfinite(tempR[i])) {
        params.R[i]  = tempR[i];
        params.R2[i] = tempR[i] / 20;
      } else {
        ROS_ERROR_STREAM("CvxWrapper - R matrix has to be PSD - parameter " << i << " !!! Safe value of 500 set instead");
        params.R[i]  = 500;
        params.R2[i] = 500 / 20;
      }
    }
  } else {
    ROS_ERROR_STREAM("CvxWrapper - R matrix wrong size " << tempR.size() << " !!! Safe values set instead");
    params.R[0]  = 500;
    params.R2[0] = 500 / 20;
  }

  if (dt <= 0 || !std::isfinite(dt)) {
    ROS_ERROR_STREAM("CvxWrapper - dt parameter wrong " << dt << " !!! Safe value of 0.01 set instead");
    dt = 0.01;
  }

  if (dt2 <= 0 || !std::isfinite(dt2)) {
    ROS_ERROR_STREAM("CvxWrapper - dt2 parameter wrong " << dt2 << " !!! Safe value of 0.2 set instead");
    dt = 0.2;
  }


  params.A[0] = 1;
  params.A[1] = 1;
  params.A[2] = 1;
  params.A[3] = dt2;
  params.A[4] = dt2;
  params.A[5] = 0.5 * dt2 * dt2;

  params.B[0] = dt2;

  params.Af[0] = 1;
  params.Af[1] = 1;
  params.Af[2] = 1;
  params.Af[3] = dt;
  params.Af[4] = dt;
  params.Af[5] = 0.5 * dt * dt;

  params.Bf[0] = dt;

  ROS_INFO("Cvx wrapper initiated");
}
void CvxWrapper::setLimits(double max_speed, double min_speed, double max_acc, double min_acc, double max_jerk, double min_jerk) {
  params.x_max_2[0] = max_speed;
  params.x_min_2[0] = min_speed;
  params.x_max_3[0] = max_acc;
  params.x_min_3[0] = min_acc;
  params.u_max[0]   = max_jerk;
  params.u_min[0]   = min_jerk;
}

void CvxWrapper::setInitialState(MatrixXd& x) {
  params.x_0[0] = x(0, 0);
  params.x_0[1] = x(1, 0);
  params.x_0[2] = x(2, 0);
}
void CvxWrapper::loadReference(MatrixXd& reference) {
  params.x_ss_1[0]  = reference(0, 0);
  params.x_ss_2[0]  = reference(1, 0);
  params.x_ss_3[0]  = reference(2, 0);
  params.x_ss_4[0]  = reference(3, 0);
  params.x_ss_5[0]  = reference(4, 0);
  params.x_ss_6[0]  = reference(5, 0);
  params.x_ss_7[0]  = reference(6, 0);
  params.x_ss_8[0]  = reference(7, 0);
  params.x_ss_9[0]  = reference(8, 0);
  params.x_ss_10[0] = reference(9, 0);
  params.x_ss_11[0] = reference(10, 0);
  params.x_ss_12[0] = reference(11, 0);
  params.x_ss_13[0] = reference(12, 0);
  params.x_ss_14[0] = reference(13, 0);
  params.x_ss_15[0] = reference(14, 0);
  params.x_ss_16[0] = reference(15, 0);
  params.x_ss_17[0] = reference(16, 0);
  params.x_ss_18[0] = reference(17, 0);
  params.x_ss_19[0] = reference(18, 0);
  params.x_ss_20[0] = reference(19, 0);
  params.x_ss_21[0] = reference(20, 0);
  params.x_ss_22[0] = reference(21, 0);
  params.x_ss_23[0] = reference(22, 0);
  params.x_ss_24[0] = reference(23, 0);
  params.x_ss_25[0] = reference(24, 0);
  params.x_ss_26[0] = reference(25, 0);
  params.x_ss_27[0] = reference(26, 0);
  params.x_ss_28[0] = reference(27, 0);
  params.x_ss_29[0] = reference(28, 0);
  params.x_ss_30[0] = reference(29, 0);
  params.x_ss_31[0] = reference(30, 0);
  params.x_ss_32[0] = reference(31, 0);
  params.x_ss_33[0] = reference(32, 0);
  params.x_ss_34[0] = reference(33, 0);
  params.x_ss_35[0] = reference(34, 0);
  params.x_ss_36[0] = reference(35, 0);
  params.x_ss_37[0] = reference(36, 0);
  params.x_ss_38[0] = reference(37, 0);
  params.x_ss_39[0] = reference(38, 0);
  params.x_ss_40[0] = reference(39, 0);
}
int CvxWrapper::solveCvx() {
  /* ROS_INFO_STREAM("[cvxgen]: " << *params.x_max_2_1 << " druhej " << *params.x_max_2_10); */
  return solve();
}
void CvxWrapper::getStates(MatrixXd& future_traj) {
  future_traj(0 + dim + (0 * 9))  = *(vars.x_1);
  future_traj(1 + dim + (0 * 9))  = *(vars.x_1 + 1);
  future_traj(2 + dim + (0 * 9))  = *(vars.x_1 + 2);
  future_traj(0 + dim + (1 * 9))  = *(vars.x_2);
  future_traj(1 + dim + (1 * 9))  = *(vars.x_2 + 1);
  future_traj(2 + dim + (1 * 9))  = *(vars.x_2 + 2);
  future_traj(0 + dim + (2 * 9))  = *(vars.x_3);
  future_traj(1 + dim + (2 * 9))  = *(vars.x_3 + 1);
  future_traj(2 + dim + (2 * 9))  = *(vars.x_3 + 2);
  future_traj(0 + dim + (3 * 9))  = *(vars.x_4);
  future_traj(1 + dim + (3 * 9))  = *(vars.x_4 + 1);
  future_traj(2 + dim + (3 * 9))  = *(vars.x_4 + 2);
  future_traj(0 + dim + (4 * 9))  = *(vars.x_5);
  future_traj(1 + dim + (4 * 9))  = *(vars.x_5 + 1);
  future_traj(2 + dim + (4 * 9))  = *(vars.x_5 + 2);
  future_traj(0 + dim + (5 * 9))  = *(vars.x_6);
  future_traj(1 + dim + (5 * 9))  = *(vars.x_6 + 1);
  future_traj(2 + dim + (5 * 9))  = *(vars.x_6 + 2);
  future_traj(0 + dim + (6 * 9))  = *(vars.x_7);
  future_traj(1 + dim + (6 * 9))  = *(vars.x_7 + 1);
  future_traj(2 + dim + (6 * 9))  = *(vars.x_7 + 2);
  future_traj(0 + dim + (7 * 9))  = *(vars.x_8);
  future_traj(1 + dim + (7 * 9))  = *(vars.x_8 + 1);
  future_traj(2 + dim + (7 * 9))  = *(vars.x_8 + 2);
  future_traj(0 + dim + (8 * 9))  = *(vars.x_9);
  future_traj(1 + dim + (8 * 9))  = *(vars.x_9 + 1);
  future_traj(2 + dim + (8 * 9))  = *(vars.x_9 + 2);
  future_traj(0 + dim + (9 * 9))  = *(vars.x_10);
  future_traj(1 + dim + (9 * 9))  = *(vars.x_10 + 1);
  future_traj(2 + dim + (9 * 9))  = *(vars.x_10 + 2);
  future_traj(0 + dim + (10 * 9)) = *(vars.x_11);
  future_traj(1 + dim + (10 * 9)) = *(vars.x_11 + 1);
  future_traj(2 + dim + (10 * 9)) = *(vars.x_11 + 2);
  future_traj(0 + dim + (11 * 9)) = *(vars.x_12);
  future_traj(1 + dim + (11 * 9)) = *(vars.x_12 + 1);
  future_traj(2 + dim + (11 * 9)) = *(vars.x_12 + 2);
  future_traj(0 + dim + (12 * 9)) = *(vars.x_13);
  future_traj(1 + dim + (12 * 9)) = *(vars.x_13 + 1);
  future_traj(2 + dim + (12 * 9)) = *(vars.x_13 + 2);
  future_traj(0 + dim + (13 * 9)) = *(vars.x_14);
  future_traj(1 + dim + (13 * 9)) = *(vars.x_14 + 1);
  future_traj(2 + dim + (13 * 9)) = *(vars.x_14 + 2);
  future_traj(0 + dim + (14 * 9)) = *(vars.x_15);
  future_traj(1 + dim + (14 * 9)) = *(vars.x_15 + 1);
  future_traj(2 + dim + (14 * 9)) = *(vars.x_15 + 2);
  future_traj(0 + dim + (15 * 9)) = *(vars.x_16);
  future_traj(1 + dim + (15 * 9)) = *(vars.x_16 + 1);
  future_traj(2 + dim + (15 * 9)) = *(vars.x_16 + 2);
  future_traj(0 + dim + (16 * 9)) = *(vars.x_17);
  future_traj(1 + dim + (16 * 9)) = *(vars.x_17 + 1);
  future_traj(2 + dim + (16 * 9)) = *(vars.x_17 + 2);
  future_traj(0 + dim + (17 * 9)) = *(vars.x_18);
  future_traj(1 + dim + (17 * 9)) = *(vars.x_18 + 1);
  future_traj(2 + dim + (17 * 9)) = *(vars.x_18 + 2);
  future_traj(0 + dim + (18 * 9)) = *(vars.x_19);
  future_traj(1 + dim + (18 * 9)) = *(vars.x_19 + 1);
  future_traj(2 + dim + (18 * 9)) = *(vars.x_19 + 2);
  future_traj(0 + dim + (19 * 9)) = *(vars.x_20);
  future_traj(1 + dim + (19 * 9)) = *(vars.x_20 + 1);
  future_traj(2 + dim + (19 * 9)) = *(vars.x_20 + 2);
  future_traj(0 + dim + (20 * 9)) = *(vars.x_21);
  future_traj(1 + dim + (20 * 9)) = *(vars.x_21 + 1);
  future_traj(2 + dim + (20 * 9)) = *(vars.x_21 + 2);
  future_traj(0 + dim + (21 * 9)) = *(vars.x_22);
  future_traj(1 + dim + (21 * 9)) = *(vars.x_22 + 1);
  future_traj(2 + dim + (21 * 9)) = *(vars.x_22 + 2);
  future_traj(0 + dim + (22 * 9)) = *(vars.x_23);
  future_traj(1 + dim + (22 * 9)) = *(vars.x_23 + 1);
  future_traj(2 + dim + (22 * 9)) = *(vars.x_23 + 2);
  future_traj(0 + dim + (23 * 9)) = *(vars.x_24);
  future_traj(1 + dim + (23 * 9)) = *(vars.x_24 + 1);
  future_traj(2 + dim + (23 * 9)) = *(vars.x_24 + 2);
  future_traj(0 + dim + (24 * 9)) = *(vars.x_25);
  future_traj(1 + dim + (24 * 9)) = *(vars.x_25 + 1);
  future_traj(2 + dim + (24 * 9)) = *(vars.x_25 + 2);
  future_traj(0 + dim + (25 * 9)) = *(vars.x_26);
  future_traj(1 + dim + (25 * 9)) = *(vars.x_26 + 1);
  future_traj(2 + dim + (25 * 9)) = *(vars.x_26 + 2);
  future_traj(0 + dim + (26 * 9)) = *(vars.x_27);
  future_traj(1 + dim + (26 * 9)) = *(vars.x_27 + 1);
  future_traj(2 + dim + (26 * 9)) = *(vars.x_27 + 2);
  future_traj(0 + dim + (27 * 9)) = *(vars.x_28);
  future_traj(1 + dim + (27 * 9)) = *(vars.x_28 + 1);
  future_traj(2 + dim + (27 * 9)) = *(vars.x_28 + 2);
  future_traj(0 + dim + (28 * 9)) = *(vars.x_29);
  future_traj(1 + dim + (28 * 9)) = *(vars.x_29 + 1);
  future_traj(2 + dim + (28 * 9)) = *(vars.x_29 + 2);
  future_traj(0 + dim + (29 * 9)) = *(vars.x_30);
  future_traj(1 + dim + (29 * 9)) = *(vars.x_30 + 1);
  future_traj(2 + dim + (29 * 9)) = *(vars.x_30 + 2);
  future_traj(0 + dim + (30 * 9)) = *(vars.x_31);
  future_traj(1 + dim + (30 * 9)) = *(vars.x_31 + 1);
  future_traj(2 + dim + (30 * 9)) = *(vars.x_31 + 2);
  future_traj(0 + dim + (31 * 9)) = *(vars.x_32);
  future_traj(1 + dim + (31 * 9)) = *(vars.x_32 + 1);
  future_traj(2 + dim + (31 * 9)) = *(vars.x_32 + 2);
  future_traj(0 + dim + (32 * 9)) = *(vars.x_33);
  future_traj(1 + dim + (32 * 9)) = *(vars.x_33 + 1);
  future_traj(2 + dim + (32 * 9)) = *(vars.x_33 + 2);
  future_traj(0 + dim + (33 * 9)) = *(vars.x_34);
  future_traj(1 + dim + (33 * 9)) = *(vars.x_34 + 1);
  future_traj(2 + dim + (33 * 9)) = *(vars.x_34 + 2);
  future_traj(0 + dim + (34 * 9)) = *(vars.x_35);
  future_traj(1 + dim + (34 * 9)) = *(vars.x_35 + 1);
  future_traj(2 + dim + (34 * 9)) = *(vars.x_35 + 2);
  future_traj(0 + dim + (35 * 9)) = *(vars.x_36);
  future_traj(1 + dim + (35 * 9)) = *(vars.x_36 + 1);
  future_traj(2 + dim + (35 * 9)) = *(vars.x_36 + 2);
  future_traj(0 + dim + (36 * 9)) = *(vars.x_37);
  future_traj(1 + dim + (36 * 9)) = *(vars.x_37 + 1);
  future_traj(2 + dim + (36 * 9)) = *(vars.x_37 + 2);
  future_traj(0 + dim + (37 * 9)) = *(vars.x_38);
  future_traj(1 + dim + (37 * 9)) = *(vars.x_38 + 1);
  future_traj(2 + dim + (37 * 9)) = *(vars.x_38 + 2);
  future_traj(0 + dim + (38 * 9)) = *(vars.x_39);
  future_traj(1 + dim + (38 * 9)) = *(vars.x_39 + 1);
  future_traj(2 + dim + (38 * 9)) = *(vars.x_39 + 2);
  future_traj(0 + dim + (39 * 9)) = *(vars.x_40);
  future_traj(1 + dim + (39 * 9)) = *(vars.x_40 + 1);
  future_traj(2 + dim + (39 * 9)) = *(vars.x_40 + 2);
}
double CvxWrapper::getFirstControlInput() {
  return *(vars.u_0);
}
