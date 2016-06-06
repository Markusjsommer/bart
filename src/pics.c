/* Copyright 2013-2015. The Regents of the University of California.
 * Copyright 2015. Martin Uecker.
 * All rights reserved. Use of this source code is governed by
 * a BSD-style license which can be found in the LICENSE file.
 *
 * Authors:
 * 2012-2015 Martin Uecker <martin.uecker@med.uni-goettingen.de>
 * 2014-2016 Frank Ong <frankong@berkeley.edu>
 * 2014-2015 Jonathan Tamir <jtamir@eecs.berkeley.edu>
 *
 */

#include <assert.h>
#include <stdbool.h>
#include <complex.h>
#include <stdio.h>
#include <math.h>

#include "num/multind.h"
#include "num/flpmath.h"
#include "num/fft.h"
#include "num/init.h"
#include "num/ops.h"
#include "num/iovec.h"

#include "iter/lsqr.h"
#include "iter/prox.h"
#include "iter/thresh.h"
#include "iter/misc.h"

#include "linops/linop.h"
#include "linops/someops.h"
#include "linops/grad.h"
#include "linops/sum.h"

#include "iter/iter.h"
#include "iter/iter2.h"

#include "noncart/nufft.h"

#include "sense/recon.h"
#include "sense/model.h"
#include "sense/optcom.h"

#include "wavelet2/wavelet.h"
#include "wavelet3/wavthresh.h"

#include "lowrank/lrthresh.h"

#include "misc/debug.h"
#include "misc/mri.h"
#include "misc/utils.h"
#include "misc/mmio.h"
#include "misc/misc.h"
#include "misc/opts.h"

#include "grecon/optreg.h"



static const char usage_str[] = "<kspace> <sensitivities> <output>";
static const char help_str[] = "Parallel-imaging compressed-sensing reconstruction.";



static const struct linop_s* sense_nc_init(const long max_dims[DIMS], const long map_dims[DIMS], const complex float* maps, const long ksp_dims[DIMS], const long traj_dims[DIMS], const complex float* traj, struct nufft_conf_s conf, bool use_gpu, struct operator_s** precond_op)
{
	long coilim_dims[DIMS];
	long img_dims[DIMS];
	md_select_dims(DIMS, ~MAPS_FLAG, coilim_dims, max_dims);
	md_select_dims(DIMS, ~COIL_FLAG, img_dims, max_dims);

	const struct linop_s* fft_op = nufft_create(DIMS, ksp_dims, coilim_dims, traj_dims, traj, NULL, conf, use_gpu);
	const struct linop_s* maps_op = maps2_create(coilim_dims, map_dims, img_dims, maps, use_gpu);

	//precond_op[0] = (struct operator_s*) nufft_precond_create( fft_op );
	precond_op[0] = NULL;

	const struct linop_s* lop = linop_chain(maps_op, fft_op);

	linop_free(maps_op);
	linop_free(fft_op);

	return lop;
}


int main_pics(int argc, char* argv[])
{
	// Initialize default parameters

	struct sense_conf conf = sense_defaults;



	bool randshift = true;
	unsigned int maxiter = 30;
	float step = -1.;

	// Start time count

	double start_time = timestamp();

	// Read input options
	struct nufft_conf_s nuconf = nufft_conf_defaults;
	nuconf.toeplitz = false;

	float restrict_fov = -1.;
	const char* pat_file = NULL;
	const char* traj_file = NULL;
	bool scale_im = false;
	bool eigen = false;
	float scaling = 0.;

	unsigned int llr_blk = 8;

	const char* image_truth_file = NULL;
	bool im_truth = false;

	const char* image_start_file = NULL;
	bool warm_start = false;

	bool hogwild = false;
	bool fast = false;
	float admm_rho = iter_admm_defaults.rho;
	unsigned int admm_maxitercg = iter_admm_defaults.maxitercg;

	struct opt_reg_s ropts;
	assert(0 == opt_reg_init(&ropts));


	const struct opt_s opts[] = {

		{ 'l', true, opt_reg, &ropts, "1/-l2\t\ttoggle l1-wavelet or l2 regularization." },
		OPT_FLOAT('r', &ropts.lambda, "lambda", "regularization parameter"),
		{ 'R', true, opt_reg, &ropts, " <T>:A:B:C\tgeneralized regularization options (-Rh for help)" },
		OPT_SET('c', &conf.rvc, "real-value constraint"),
		OPT_FLOAT('s', &step, "step", "iteration stepsize"),
		OPT_UINT('i', &maxiter, "iter", "max. number of iterations"),
		OPT_STRING('t', &traj_file, "file", "k-space trajectory"),
		OPT_CLEAR('n', &randshift, "disable random wavelet cycle spinning"),
		OPT_SET('g', &conf.gpu, "use GPU"),
		OPT_STRING('p', &pat_file, "file", "pattern or weights"),
		OPT_SELECT('I', enum algo_t, &ropts.algo, IST, "(select IST)"),
		OPT_UINT('b', &llr_blk, "blk", "Lowrank block size"),
		OPT_SET('e', &eigen, "Scale stepsize based on max. eigenvalue"),
		OPT_SET('H', &hogwild, "(hogwild)"),
		OPT_SET('F', &fast, "(fast)"),
		OPT_STRING('T', &image_truth_file, "file", "(truth file)"),
		OPT_STRING('W', &image_start_file, "<img>", "Warm start with <img>"),
		OPT_INT('d', &debug_level, "level", "Debug level"),
		OPT_INT('O', &conf.rwiter, "rwiter", "(reweighting)"),
		OPT_FLOAT('o', &conf.gamma, "gamma", "(reweighting)"),
		OPT_FLOAT('u', &admm_rho, "rho", "ADMM rho"),
		OPT_UINT('C', &admm_maxitercg, "iter", "ADMM max. CG iterations"),
		OPT_FLOAT('q', &conf.cclambda, "cclambda", "(cclambda)"),
		OPT_FLOAT('f', &restrict_fov, "rfov", "restrict FOV"),
		OPT_SELECT('m', enum algo_t, &ropts.algo, ADMM, "Select ADMM"),
		OPT_FLOAT('w', &scaling, "val", "scaling"),
		OPT_SET('S', &scale_im, "Re-scale the image after reconstruction"),
	};

	cmdline(&argc, argv, 3, 3, usage_str, help_str, ARRAY_SIZE(opts), opts);

	if (NULL != image_truth_file)
		im_truth = true;

	if (NULL != image_start_file)
		warm_start = true;


	long max_dims[DIMS];
	long map_dims[DIMS];
	long pat_dims[DIMS];
	long img_dims[DIMS];
	long coilim_dims[DIMS];
	long ksp_dims[DIMS];
	long traj_dims[DIMS];



	// load kspace and maps and get dimensions

	complex float* kspace = load_cfl(argv[1], DIMS, ksp_dims);
	complex float* maps = load_cfl(argv[2], DIMS, map_dims);


	complex float* traj = NULL;

	if (NULL != traj_file)
		traj = load_cfl(traj_file, DIMS, traj_dims);


	md_copy_dims(DIMS, max_dims, ksp_dims);
	md_copy_dims(5, max_dims, map_dims);

	md_select_dims(DIMS, ~COIL_FLAG, img_dims, max_dims);
	md_select_dims(DIMS, ~MAPS_FLAG, coilim_dims, max_dims);

	if (!md_check_compat(DIMS, ~(MD_BIT(MAPS_DIM)|FFT_FLAGS), img_dims, map_dims))
		error("Dimensions of image and sensitivities do not match!\n");

	assert(1 == ksp_dims[MAPS_DIM]);


	(conf.gpu ? num_init_gpu : num_init)();

	// print options

	if (conf.gpu)
		debug_printf(DP_INFO, "GPU reconstruction\n");

	if (map_dims[MAPS_DIM] > 1) 
		debug_printf(DP_INFO, "%ld maps.\nESPIRiT reconstruction.\n", map_dims[MAPS_DIM]);

	if (hogwild)
		debug_printf(DP_INFO, "Hogwild stepsize\n");

	if (im_truth)
		debug_printf(DP_INFO, "Compare to truth\n");



	// initialize sampling pattern

	complex float* pattern = NULL;

	if (NULL != pat_file) {

		pattern = load_cfl(pat_file, DIMS, pat_dims);

		assert(md_check_compat(DIMS, COIL_FLAG, ksp_dims, pat_dims));

	} else {

		md_select_dims(DIMS, ~COIL_FLAG, pat_dims, ksp_dims);
		pattern = md_alloc(DIMS, pat_dims, CFL_SIZE);
		estimate_pattern(DIMS, ksp_dims, COIL_FLAG, pattern, kspace);
	}


	if ((NULL != traj_file) && (NULL == pat_file)) {

		md_free(pattern);
		pattern = NULL;
		nuconf.toeplitz = true;

	} else {

		// print some statistics

		long T = md_calc_size(DIMS, pat_dims);
		long samples = (long)pow(md_znorm(DIMS, pat_dims, pattern), 2.);

		debug_printf(DP_INFO, "Size: %ld Samples: %ld Acc: %.2f\n", T, samples, (float)T / (float)samples);
	}

	if (NULL == traj_file) {

		fftmod(DIMS, ksp_dims, FFT_FLAGS, kspace, kspace);
		fftmod(DIMS, map_dims, FFT_FLAGS, maps, maps);
	}

	// apply fov mask to sensitivities

	if (-1. != restrict_fov) {

		float restrict_dims[DIMS] = { [0 ... DIMS - 1] = 1. };
		restrict_dims[0] = restrict_fov;
		restrict_dims[1] = restrict_fov;
		restrict_dims[2] = restrict_fov;

		apply_mask(DIMS, map_dims, maps, restrict_dims);
	}


	// initialize forward_op and precond_op

	const struct linop_s* forward_op = NULL;
	const struct operator_s* precond_op = NULL;

	if (NULL == traj_file)
		forward_op = sense_init(max_dims, FFT_FLAGS|COIL_FLAG|MAPS_FLAG, maps, conf.gpu);
	else
		forward_op = sense_nc_init(max_dims, map_dims, maps, ksp_dims, traj_dims, traj, nuconf, conf.gpu, (struct operator_s**) &precond_op);

	// apply scaling

	if (scaling == 0.) {

		if (NULL == traj_file) {

			scaling = estimate_scaling(ksp_dims, NULL, kspace);

		} else {

			complex float* adj = md_alloc(DIMS, img_dims, CFL_SIZE);

			linop_adjoint(forward_op, DIMS, img_dims, adj, DIMS, ksp_dims, kspace);
			scaling = estimate_scaling_norm(1., md_calc_size(DIMS, img_dims), adj, false);

			md_free(adj);
		}
	}
	else
		debug_printf(DP_DEBUG1, "Scaling: %f\n", scaling);

	if (scaling != 0.)
		md_zsmul(DIMS, ksp_dims, kspace, kspace, 1. / scaling);


	// initialize prox functions
	const struct operator_p_s* thresh_ops[NUM_REGS] = { NULL };
	const struct linop_s* trafos[NUM_REGS] = { NULL };

	opt_reg_configure(DIMS, img_dims, &ropts, thresh_ops, trafos, llr_blk, randshift, conf.gpu);

	int nr_penalties = ropts.r;
	struct reg_s* regs = ropts.regs;
	enum algo_t algo = ropts.algo;


	complex float* image = create_cfl(argv[3], DIMS, img_dims);
	md_clear(DIMS, img_dims, image, CFL_SIZE);


	long img_truth_dims[DIMS];
	complex float* image_truth = NULL;

	if (im_truth) {

		image_truth = load_cfl(image_truth_file, DIMS, img_truth_dims);
		//md_zsmul(DIMS, img_dims, image_truth, image_truth, 1. / scaling);
	}

	long img_start_dims[DIMS];
	complex float* image_start = NULL;

	if (warm_start) { 

		debug_printf(DP_DEBUG1, "Warm start: %s\n", image_start_file);
		image_start = load_cfl(image_start_file, DIMS, img_start_dims);
		assert(md_check_compat(DIMS, 0u, img_start_dims, img_dims));
		md_copy(DIMS, img_dims, image, image_start, CFL_SIZE);

		free((void*)image_start_file);
		unmap_cfl(DIMS, img_dims, image_start);

		// if rescaling at the end, assume the input has also been rescaled
		if (scale_im && scaling != 0.)
			md_zsmul(DIMS, img_dims, image, image, 1. /  scaling);
	}


	// initialize algorithm

	italgo_fun2_t italgo = iter2_call_iter;
	struct iter_call_s iter2_data;

	iter_conf* iconf = &iter2_data.base;

	struct iter_conjgrad_conf cgconf;
	struct iter_fista_conf fsconf;
	struct iter_ist_conf isconf;
	struct iter_admm_conf mmconf;

	if ((CG == algo) && (1 == nr_penalties) && (L2IMG != regs[0].xform))
		algo = FISTA;

	if (nr_penalties > 1)
		algo = ADMM;

	if ((IST == algo) || (FISTA == algo)) {

		// For non-Cartesian trajectories, the default
		// will usually not work. TODO: The same is true
		// for sensitivities which are not normalized, but
		// we do not detect this case.

		if ((NULL != traj_file) && (-1. == step) && !eigen)
			debug_printf(DP_WARN, "No step size specified.\n");

		if (-1. == step)
			step = 0.95;
	}

	if ((CG == algo) || (ADMM == algo))
		if (-1. != step)
			debug_printf(DP_INFO, "Stepsize ignored.\n");

	if (eigen) {

		double maxeigen = estimate_maxeigenval(forward_op->normal);

		debug_printf(DP_INFO, "Maximum eigenvalue: %.2e\n", maxeigen);

		step /= maxeigen;
	}

	switch (algo) {

		case CG:

			debug_printf(DP_INFO, "conjugate gradients\n");

			assert((0 == nr_penalties) || ((1 == nr_penalties) && (L2IMG == regs[0].xform)));

			cgconf = iter_conjgrad_defaults;
			cgconf.maxiter = maxiter;
			cgconf.l2lambda = (0 == nr_penalties) ? 0. : regs[0].lambda;

			iter2_data.fun = iter_conjgrad;
			iter2_data._conf = &cgconf.base;

			nr_penalties = 0;

			break;

		case IST:

			debug_printf(DP_INFO, "IST\n");

			assert(1 == nr_penalties);

			isconf = iter_ist_defaults;
			isconf.maxiter = maxiter;
			isconf.step = step;
			isconf.hogwild = hogwild;

			iter2_data.fun = iter_ist;
			iter2_data._conf = &isconf.base;

			break;

		case ADMM:

			debug_printf(DP_INFO, "ADMM\n");

			mmconf = iter_admm_defaults;
			mmconf.maxiter = maxiter;
			mmconf.maxitercg = admm_maxitercg;
			mmconf.rho = admm_rho;
			mmconf.hogwild = hogwild;
			mmconf.fast = fast;
			//		mmconf.dynamic_rho = true;
			mmconf.ABSTOL = 0.;
			mmconf.RELTOL = 0.;

			italgo = iter2_admm;
			iconf = &mmconf.base;

			break;

		case FISTA:

			debug_printf(DP_INFO, "FISTA\n");

			assert(1 == nr_penalties);

			fsconf = iter_fista_defaults;
			fsconf.maxiter = maxiter;
			fsconf.step = step;
			fsconf.hogwild = hogwild;

			iter2_data.fun = iter_fista;
			iter2_data._conf = &fsconf.base;

			break;

		default:

			assert(0);
	}



	const struct operator_s* op = sense_recon_create(&conf, max_dims, forward_op, pat_dims, pattern,
				italgo, iconf, nr_penalties, thresh_ops,
				(ADMM == algo) ? trafos : NULL, ksp_dims, precond_op);

	if (conf.gpu)
#ifdef USE_CUDA
		op = operator_gpu_wrapper(op);
#else
		assert(0);
#endif

	operator_apply(op, DIMS, img_dims, image, DIMS, ksp_dims, kspace);

	operator_free(op);



	if (scale_im)
		md_zsmul(DIMS, img_dims, image, image, scaling);

	// clean up

	if (NULL != pat_file)
		unmap_cfl(DIMS, pat_dims, pattern);
	else
		md_free(pattern);


	unmap_cfl(DIMS, map_dims, maps);
	unmap_cfl(DIMS, ksp_dims, kspace);
	unmap_cfl(DIMS, img_dims, image);

	if (NULL != traj)
		unmap_cfl(DIMS, traj_dims, traj);

	if (im_truth) {

		free((void*)image_truth_file);
		unmap_cfl(DIMS, img_dims, image_truth);
	}


	double end_time = timestamp();

	debug_printf(DP_INFO, "Total Time: %f\n", end_time - start_time);
	exit(0);
}


