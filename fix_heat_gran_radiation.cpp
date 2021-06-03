/* ----------------------------------------------------------------------
    This is the

    ██╗     ██╗ ██████╗  ██████╗  ██████╗ ██╗  ██╗████████╗███████╗
    ██║     ██║██╔════╝ ██╔════╝ ██╔════╝ ██║  ██║╚══██╔══╝██╔════╝
    ██║     ██║██║  ███╗██║  ███╗██║  ███╗███████║   ██║   ███████╗
    ██║     ██║██║   ██║██║   ██║██║   ██║██╔══██║   ██║   ╚════██║
    ███████╗██║╚██████╔╝╚██████╔╝╚██████╔╝██║  ██║   ██║   ███████║
    ╚══════╝╚═╝ ╚═════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝   ╚═╝   ╚══════╝®

    DEM simulation engine, released by
    DCS Computing Gmbh, Linz, Austria
    http://www.dcs-computing.com, office@dcs-computing.com

    LIGGGHTS® is part of CFDEM®project:
    http://www.liggghts.com | http://www.cfdem.com

    Core developer and main author:
    Christoph Kloss, christoph.kloss@dcs-computing.com

    LIGGGHTS® is open-source, distributed under the terms of the GNU Public
    License, version 2 or later. It is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. You should have
    received a copy of the GNU General Public License along with LIGGGHTS®.
    If not, see http://www.gnu.org/licenses . See also top-level README
    and LICENSE files.

    LIGGGHTS® and CFDEM® are registered trade marks of DCS Computing GmbH,
    the producer of the LIGGGHTS® software and the CFDEM®coupling software
    See http://www.cfdem.com/terms-trademark-policy for details.

-------------------------------------------------------------------------
    Contributing author and copyright for this file:
    (if not contributing author is listed, this file has been contributed
    by the core developer)

    Copyright 2012-     DCS Computing GmbH, Linz
    Copyright 2009-2012 JKU Linz
------------------------------------------------------------------------- */

#include "fix_heat_gran_radiation.h"

#include "atom.h"
#include "compute_pair_gran_local.h"
#include "fix_property_atom.h"
#include "fix_property_global.h"
#include "force.h"
#include "math_extra.h"
#include "properties.h"
#include "modify.h"
#include "neigh_list.h"
#include "pair_gran.h"
#include <cmath>
#include <algorithm>
#define STEFAN_BOLTZMANN 5.67e-8

using namespace LAMMPS_NS;
using namespace FixConst;

// modes for conduction contact area calaculation
// same as in fix_wall_gran.cpp

enum{ CONDUCTION_CONTACT_AREA_OVERLAP,
      CONDUCTION_CONTACT_AREA_CONSTANT,
      CONDUCTION_CONTACT_AREA_PROJECTION};

/* ---------------------------------------------------------------------- */

FixHeatGranRad::FixHeatGranRad(class LAMMPS *lmp, int narg, char **arg) :
  FixHeatGran(lmp, narg, arg),
  fix_conductivity_(0),
  conductivity_(0),
  store_contact_data_(false),
  fix_conduction_contact_area_(0),
  fix_n_conduction_contacts_(0),
  fix_wall_heattransfer_coeff_(0),
  fix_wall_temperature_(0),
  conduction_contact_area_(0),
  n_conduction_contacts_(0),
  wall_heattransfer_coeff_(0),
  wall_temp_(0),
  area_calculation_mode_(CONDUCTION_CONTACT_AREA_OVERLAP),
  fixed_contact_area_(0.),
  area_correction_flag_(0),
  deltan_ratio_(0)
{
  iarg_ = 5;

  bool hasargs = true;
  while(iarg_ < narg && hasargs)
  {
    hasargs = false;
    if(strcmp(arg[iarg_],"contact_area") == 0) {

      if(strcmp(arg[iarg_+1],"overlap") == 0)
        area_calculation_mode_ =  CONDUCTION_CONTACT_AREA_OVERLAP;
      else if(strcmp(arg[iarg_+1],"projection") == 0)
        area_calculation_mode_ =  CONDUCTION_CONTACT_AREA_PROJECTION;
      else if(strcmp(arg[iarg_+1],"constant") == 0)
      {
        if (iarg_+3 > narg)
            error->fix_error(FLERR,this,"not enough arguments for keyword 'contact_area constant'");
        area_calculation_mode_ =  CONDUCTION_CONTACT_AREA_CONSTANT;
        fixed_contact_area_ = force->numeric(FLERR,arg[iarg_+2]);
        if (fixed_contact_area_ <= 0.)
            error->fix_error(FLERR,this,"'contact_area constant' value must be > 0");
        iarg_++;
      }
      else error->fix_error(FLERR,this,"expecting 'overlap', 'projection' or 'constant' after 'contact_area'");
      iarg_ += 2;
      hasargs = true;
    } else if(strcmp(arg[iarg_],"area_correction") == 0) {
      if (iarg_+2 > narg) error->fix_error(FLERR,this,"not enough arguments for keyword 'area_correction'");
      if(strcmp(arg[iarg_+1],"yes") == 0)
        area_correction_flag_ = 1;
      else if(strcmp(arg[iarg_+1],"no") == 0)
        area_correction_flag_ = 0;
      else error->fix_error(FLERR,this,"expecting 'yes' or 'no' after 'area_correction'");
      iarg_ += 2;
      hasargs = true;
    } else if(strcmp(arg[iarg_],"store_contact_data") == 0) {
      if (iarg_+2 > narg) error->fix_error(FLERR,this,"not enough arguments for keyword 'store_contact_data'");
      if(strcmp(arg[iarg_+1],"yes") == 0)
        store_contact_data_ = true;
      else if(strcmp(arg[iarg_+1],"no") == 0)
        store_contact_data_ = false;
      else error->fix_error(FLERR,this,"expecting 'yes' or 'no' after 'store_contact_data'");
      iarg_ += 2;
      hasargs = true;
    } else if(strcmp(style,"heat/gran/conduction") == 0)
        error->fix_error(FLERR,this,"unknown keyword");
  }

  if(CONDUCTION_CONTACT_AREA_OVERLAP != area_calculation_mode_ && 1 == area_correction_flag_)
    error->fix_error(FLERR,this,"can use 'area_correction' only for 'contact_area = overlap'");
}

/* ---------------------------------------------------------------------- */

FixHeatGranRad::~FixHeatGranRad()
{

  if (conductivity_)
    delete []conductivity_;
}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::post_create()
{
  FixHeatGran::post_create();

  // register contact storage
  fix_conduction_contact_area_ = static_cast<FixPropertyAtom*>(modify->find_fix_property("contactAreaConduction","property/atom","scalar",0,0,this->style,false));
  if(!fix_conduction_contact_area_ && store_contact_data_)
  {
    const char* fixarg[10];
    fixarg[0]="contactAreaConduction";
    fixarg[1]="all";
    fixarg[2]="property/atom";
    fixarg[3]="contactAreaConduction";
    fixarg[4]="scalar";
    fixarg[5]="no";
    fixarg[6]="yes";
    fixarg[7]="no";
    fixarg[8]="0.";
    fix_conduction_contact_area_ = modify->add_fix_property_atom(9,const_cast<char**>(fixarg),style);
  }

  fix_n_conduction_contacts_ = static_cast<FixPropertyAtom*>(modify->find_fix_property("nContactsConduction","property/atom","scalar",0,0,this->style,false));
  if(!fix_n_conduction_contacts_ && store_contact_data_)
  {
    const char* fixarg[10];
    fixarg[0]="nContactsConduction";
    fixarg[1]="all";
    fixarg[2]="property/atom";
    fixarg[3]="nContactsConduction";
    fixarg[4]="scalar";
    fixarg[5]="no";
    fixarg[6]="yes";
    fixarg[7]="no";
    fixarg[8]="0.";
    fix_n_conduction_contacts_ = modify->add_fix_property_atom(9,const_cast<char**>(fixarg),style);
  }

  fix_wall_heattransfer_coeff_ = static_cast<FixPropertyAtom*>(modify->find_fix_property("wallHeattransferCoeff","property/atom","scalar",0,0,this->style,false));
  if(!fix_wall_heattransfer_coeff_ && store_contact_data_)
  {
    const char* fixarg[10];
    fixarg[0]="wallHeattransferCoeff";
    fixarg[1]="all";
    fixarg[2]="property/atom";
    fixarg[3]="wallHeattransferCoeff";
    fixarg[4]="scalar";
    fixarg[5]="no";
    fixarg[6]="yes";
    fixarg[7]="no";
    fixarg[8]="0.";
    fix_wall_heattransfer_coeff_ = modify->add_fix_property_atom(9,const_cast<char**>(fixarg),style);
  }

  fix_wall_temperature_ = static_cast<FixPropertyAtom*>(modify->find_fix_property("wallTemp","property/atom","scalar",0,0,this->style,false));
  if(!fix_wall_temperature_ && store_contact_data_)
  {
    const char* fixarg[10];
    fixarg[0]="wallTemp";
    fixarg[1]="all";
    fixarg[2]="property/atom";
    fixarg[3]="wallTemp";
    fixarg[4]="scalar";
    fixarg[5]="no";
    fixarg[6]="yes";
    fixarg[7]="no";
    fixarg[8]="0.";
    fix_wall_temperature_ = modify->add_fix_property_atom(9,const_cast<char**>(fixarg),style);
  }

  if(store_contact_data_ && (!fix_conduction_contact_area_ || !fix_n_conduction_contacts_ || !fix_wall_heattransfer_coeff_ || !fix_wall_temperature_))
    error->one(FLERR,"internal error");
}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::pre_delete(bool unfixflag)
{

  // tell cpl that this fix is deleted
  if(cpl && unfixflag) cpl->reference_deleted();

}

/* ---------------------------------------------------------------------- */

int FixHeatGranRad::setmask()
{
  int mask = FixHeatGran::setmask();
  mask |= PRE_FORCE;
  mask |= POST_FORCE;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::updatePtrs()
{
  FixHeatGran::updatePtrs();

  if(store_contact_data_)
  {
    conduction_contact_area_ = fix_conduction_contact_area_->vector_atom;
    n_conduction_contacts_ = fix_n_conduction_contacts_->vector_atom;
    wall_heattransfer_coeff_ = fix_wall_heattransfer_coeff_->vector_atom;
    wall_temp_ = fix_wall_temperature_->vector_atom;
  }
}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::init()
{
  // initialize base class
  FixHeatGran::init();

  const double *Y, *nu, *Y_orig;
  double expo, Yeff_ij, Yeff_orig_ij, ratio;
  int max_type = atom->get_properties()->max_type();
  int nlocal = atom->nlocal;

  if (conductivity_) delete []conductivity_;
  if (atom->cond) delete []atom->cond;
  conductivity_ = new double[max_type];
  atom->cond = new double[nlocal];
  fix_conductivity_ =
    static_cast<FixPropertyGlobal*>(modify->find_fix_property("thermalConductivity","property/global","peratomtype",max_type,0,style));

  // pre-calculate conductivity for possible contact material combinations
  double conductivity_nontempdep;
  for(int i=1;i< max_type+1; i++)
      for(int j=1;j<max_type+1;j++)
      {
          conductivity_nontempdep = fix_conductivity_->compute_vector(i-1);
          conductivity_[i-1] = conductivity_nontempdep;
          //conductivity_[i-1] = fix_conductivity_->compute_vector(i-1);
          atom->cond[i-1]=conductivity_[i-1];
          //printf("Conductivity:%.2f",conductivity_[i-1]);
          //printf("Maxtype:%d",max_type);
          if(conductivity_[i-1] < 0.)
            error->all(FLERR,"Fix heat/gran/conduction: Thermal conductivity must not be < 0");
      }
     
  double* cond;
  cond = new double[nlocal];
  for(int i=0;i<nlocal;i++)
  {
    cond[i]=conductivity_nontempdep + 0.000425*(Temp[i]-273.15);
    cond[i]=conductivity_nontempdep;
    atom->cond[i]=cond[i];
  }
  delete[] cond;
 /*  
  //atom->cond = conductivity_;
  
  double cond_sum = 0.;
  for(int i=1;i< max_type+1; i++)
    {
      cond_sum=cond_sum+conductivity_[i-1];
    }
  double cond_mean = cond_sum/max_type;
  atom->cond = cond_mean;
  */
  // calculate heat transfer correction

  if(area_correction_flag_)
  {
    if(!force->pair_match("gran",0))
        error->fix_error(FLERR,this,"area correction only works with using granular pair styles");

    expo = 1./pair_gran->stressStrainExponent();

    Y = static_cast<FixPropertyGlobal*>(modify->find_fix_property("youngsModulus","property/global","peratomtype",max_type,0,style))->get_values();
    nu = static_cast<FixPropertyGlobal*>(modify->find_fix_property("poissonsRatio","property/global","peratomtype",max_type,0,style))->get_values();
    Y_orig = static_cast<FixPropertyGlobal*>(modify->find_fix_property("youngsModulusOriginal","property/global","peratomtype",max_type,0,style))->get_values();

    // allocate a new array within youngsModulusOriginal
    static_cast<FixPropertyGlobal*>(modify->find_fix_property("youngsModulusOriginal","property/global","peratomtype",max_type,0,style))->new_array(max_type,max_type);

    // feed deltan_ratio into this array
    for(int i = 1; i < max_type+1; i++)
    {
      for(int j = 1; j < max_type+1; j++)
      {
        Yeff_ij      = 1./((1.-pow(nu[i-1],2.))/Y[i-1]     +(1.-pow(nu[j-1],2.))/Y[j-1]);
        Yeff_orig_ij = 1./((1.-pow(nu[i-1],2.))/Y_orig[i-1]+(1.-pow(nu[j-1],2.))/Y_orig[j-1]);
        ratio = pow(Yeff_ij/Yeff_orig_ij,expo);
        
        static_cast<FixPropertyGlobal*>(modify->find_fix_property("youngsModulusOriginal","property/global","peratomtype",max_type,0,style))->array_modify(i-1,j-1,ratio);
      }
    }

    // get reference to deltan_ratio
    deltan_ratio_ = static_cast<FixPropertyGlobal*>(modify->find_fix_property("youngsModulusOriginal","property/global","peratomtype",max_type,0,style))->get_array_modified();
  }

  updatePtrs();

  // error checks on coarsegraining
  
}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::pre_force(int vflag)
{
    
    if(store_contact_data_)
    {
        fix_wall_heattransfer_coeff_->set_all(0.);
        fix_wall_temperature_->set_all(0.);
    }
}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::post_force(int vflag)
{

  if(history_flag == 0 && CONDUCTION_CONTACT_AREA_OVERLAP == area_calculation_mode_)
    post_force_eval<0,CONDUCTION_CONTACT_AREA_OVERLAP>(vflag,0);
  if(history_flag == 1 && CONDUCTION_CONTACT_AREA_OVERLAP == area_calculation_mode_)
    post_force_eval<1,CONDUCTION_CONTACT_AREA_OVERLAP>(vflag,0);

  if(history_flag == 0 && CONDUCTION_CONTACT_AREA_CONSTANT == area_calculation_mode_)
    post_force_eval<0,CONDUCTION_CONTACT_AREA_CONSTANT>(vflag,0);
  if(history_flag == 1 && CONDUCTION_CONTACT_AREA_CONSTANT == area_calculation_mode_)
    post_force_eval<1,CONDUCTION_CONTACT_AREA_CONSTANT>(vflag,0);

  if(history_flag == 0 && CONDUCTION_CONTACT_AREA_PROJECTION == area_calculation_mode_)
    post_force_eval<0,CONDUCTION_CONTACT_AREA_PROJECTION>(vflag,0);
  if(history_flag == 1 && CONDUCTION_CONTACT_AREA_PROJECTION == area_calculation_mode_)
    post_force_eval<1,CONDUCTION_CONTACT_AREA_PROJECTION>(vflag,0);
}

/* ---------------------------------------------------------------------- */

void FixHeatGranRad::cpl_evaluate(ComputePairGranLocal *caller)
{
  if(caller != cpl) error->all(FLERR,"Illegal situation in FixHeatGranRad::cpl_evaluate");

  if(history_flag == 0 && CONDUCTION_CONTACT_AREA_OVERLAP == area_calculation_mode_)
    post_force_eval<0,CONDUCTION_CONTACT_AREA_OVERLAP>(0,1);
  if(history_flag == 1 && CONDUCTION_CONTACT_AREA_OVERLAP == area_calculation_mode_)
    post_force_eval<1,CONDUCTION_CONTACT_AREA_OVERLAP>(0,1);

  if(history_flag == 0 && CONDUCTION_CONTACT_AREA_CONSTANT == area_calculation_mode_)
    post_force_eval<0,CONDUCTION_CONTACT_AREA_CONSTANT>(0,1);
  if(history_flag == 1 && CONDUCTION_CONTACT_AREA_CONSTANT == area_calculation_mode_)
    post_force_eval<1,CONDUCTION_CONTACT_AREA_CONSTANT>(0,1);

  if(history_flag == 0 && CONDUCTION_CONTACT_AREA_PROJECTION == area_calculation_mode_)
    post_force_eval<0,CONDUCTION_CONTACT_AREA_PROJECTION>(0,1);
  if(history_flag == 1 && CONDUCTION_CONTACT_AREA_PROJECTION == area_calculation_mode_)
    post_force_eval<1,CONDUCTION_CONTACT_AREA_PROJECTION>(0,1);
}

/* ---------------------------------------------------------------------- */

template <int HISTFLAG,int CONTACTAREA>
void FixHeatGranRad::post_force_eval(int vflag,int cpl_flag)
{
  //printf("radiation\n");
  double hc,contactArea,delta_n,flux,flux2,dirFlux[3],dirFlux2[3];
  //double hc,contactArea,delta_n,flux,dirFlux[3];
  int i,j,ii,jj,inum,jnum;
  double xtmp,ytmp,ztmp,delx,dely,delz;
  double radi,radj,radsum,rsq,r,tcoi,tcoj;
  int *ilist,*jlist,*numneigh,**firstneigh;
  int *contact_flag,**first_contact_flag;

  int newton_pair = force->newton_pair;

  double disless,ViewFactor;

  if (strcmp(force->pair_style,"hybrid")==0)
    error->warning(FLERR,"Fix heat/gran/conduction implementation may not be valid for pair style hybrid");
  if (strcmp(force->pair_style,"hybrid/overlay")==0)
    error->warning(FLERR,"Fix heat/gran/conduction implementation may not be valid for pair style hybrid/overlay");

  inum = pair_gran->list->inum;
  ilist = pair_gran->list->ilist;
  numneigh = pair_gran->list->numneigh;
  firstneigh = pair_gran->list->firstneigh;
  if(HISTFLAG) first_contact_flag = pair_gran->listgranhistory->firstneigh;

  //printf("inum: %d\n",inum);

  double *radius = atom->radius;
  double **x = atom->x;
  int *type = atom->type;
  int nlocal = atom->nlocal;
  int *mask = atom->mask;

  //printf("nlocal: %d\n",nlocal);

  updatePtrs();

  if(store_contact_data_)
  {
    fix_conduction_contact_area_->set_all(0.);
    fix_n_conduction_contacts_->set_all(0.);
  }

  //FixWallGran *fwg22222223;
  //double xplane = fwg22222223->getWallLocation();
  //printf("Xplane: %.4f \n");

  //radiation heat flux compute
  //for (i = 0;i<nlocal;i++){
  //  heatFlux[i] = 0.;  
  //}

  for (i = 0;i<nlocal;i++){
    
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    radi = radius[i];
    

    for (j = i + 1;j<nlocal;j++){
      if (j != i)
      {
          delx = xtmp - x[j][0];
          dely = ytmp - x[j][1];
          delz = ztmp - x[j][2];
          rsq = (delx*delx + dely*dely + delz*delz);
          radj = radius[j];
          radsum = radi + radj;

        r = sqrt(rsq);
        disless = sqrt(rsq)/(2.*radj);
        ViewFactor = -5.2e-5+0.064/(disless*disless);
        //printf("ViewFactor:# %.4f\n",ViewFactor);
        //ViewFactor = 1./(nlocal-1);

        double tempi = Temp[i]*Temp[i]*Temp[i]*Temp[i];
        double tempj = Temp[j]*Temp[j]*Temp[j]*Temp[j];

        //printf("boltzmann constant::# %.4f\n",STEFAN_BOLTZMANN);

        double A_sphere = 4.*3.1415926*radj*radj;
        flux2 = STEFAN_BOLTZMANN*ViewFactor*(tempj-tempi)*A_sphere;
        
        //if (flux2 != 0) printf("radiation::flux::# %.4f\n",flux2);
        
        dirFlux2[0] = flux2*delx;
        dirFlux2[1] = flux2*dely;
        dirFlux2[2] = flux2*delz;

        if(!cpl_flag)
        {
        
          heatFlux[i] += flux2;
          directionalHeatFlux[i][0] += 0.50 * dirFlux2[0];
          directionalHeatFlux[i][1] += 0.50 * dirFlux2[1];
          directionalHeatFlux[i][2] += 0.50 * dirFlux2[2];

          //if (heatFlux[i] != 0) printf("radiation::heatFlux::# %.4f\n",heatFlux[i]);
          


          if (newton_pair || j < nlocal)
          {
            heatFlux[j] -= flux2;
            directionalHeatFlux[j][0] += 0.50 * dirFlux2[0];
            directionalHeatFlux[j][1] += 0.50 * dirFlux2[1];
            directionalHeatFlux[j][2] += 0.50 * dirFlux2[2];
          }
          
  
        }

        if (cpl_flag && cpl) cpl->add_heat(i,j,flux2);
        //printf("radiation::flux::# %.4f\n",heatFlux[i]);
      
      }
          
    }
    //if (heatFlux[i] != 0) printf("radiation::heatFlux::# %.4f\n",heatFlux[25]);
  }
  //printf("radiation::heatFlux::# %.18f\n",heatFlux[25]);

  // loop over neighbors of my atoms
  for (ii = 0; ii < inum; ii++) {
    i = ilist[ii];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    radi = radius[i];
    jlist = firstneigh[i];
    jnum = numneigh[i];
    if(HISTFLAG) contact_flag = first_contact_flag[i];

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      j &= NEIGHMASK;

      if (!(mask[i] & groupbit) && !(mask[j] & groupbit)) continue;

      if(!HISTFLAG)
      {
        delx = xtmp - x[j][0];
        dely = ytmp - x[j][1];
        delz = ztmp - x[j][2];
        rsq = delx*delx + dely*dely + delz*delz;
        radj = radius[j];
        radsum = radi + radj;
      }

      if ((HISTFLAG && contact_flag[jj]) || (!HISTFLAG && (rsq < radsum*radsum))) {  //contact
        
        if(HISTFLAG)
        {
          delx = xtmp - x[j][0];
          dely = ytmp - x[j][1];
          delz = ztmp - x[j][2];
          rsq = delx*delx + dely*dely + delz*delz;
          radj = radius[j];
          radsum = radi + radj;
          if(rsq >= radsum*radsum) continue;
        }

        r = sqrt(rsq);

        if(CONTACTAREA == CONDUCTION_CONTACT_AREA_OVERLAP)
        {
            
            if(area_correction_flag_)
            {
              delta_n = radsum - r;
              delta_n *= deltan_ratio_[type[i]-1][type[j]-1];
              r = radsum - delta_n;
            }

            if (r < fmax(radi, radj)) // one sphere is inside the other
            {
                // set contact area to area of smaller sphere
                contactArea = fmin(radi,radj);
                contactArea *= contactArea * M_PI;
            }
            else
                //contact area of the two spheres
                contactArea = - M_PI/4.0 * ( (r-radi-radj)*(r+radi-radj)*(r-radi+radj)*(r+radi+radj) )/(r*r);
        }
        else if (CONTACTAREA == CONDUCTION_CONTACT_AREA_CONSTANT)
            contactArea = fixed_contact_area_;
        else if (CONTACTAREA == CONDUCTION_CONTACT_AREA_PROJECTION)
        {
            double rmax = std::max(radi,radj);
            contactArea = M_PI*rmax*rmax;
        }
      
        //tcoi = conductivity_[type[i]-1];
        //tcoj = conductivity_[type[j]-1];
        tcoi = conductivity_[type[i]-1] + 0.000425*(Temp[i]-273.15);
        tcoj = conductivity_[type[j]-1] + 0.000425*(Temp[j]-273.15);
        atom->cond[i] = tcoi;
        atom->cond[j] = tcoj;
        if (tcoi < SMALL_FIX_HEAT_GRAN || tcoj < SMALL_FIX_HEAT_GRAN) hc = 0.;
        else hc = 4.*tcoi*tcoj/(tcoi+tcoj)*sqrt(contactArea);

        flux = (Temp[j]-Temp[i])*hc;

        dirFlux[0] = flux*delx;
        dirFlux[1] = flux*dely;
        dirFlux[2] = flux*delz;
        if(!cpl_flag)
        {
          //Add half of the flux (located at the contact) to each particle in contact
          heatFlux[i] += flux;
          directionalHeatFlux[i][0] += 0.50 * dirFlux[0];
          directionalHeatFlux[i][1] += 0.50 * dirFlux[1];
          directionalHeatFlux[i][2] += 0.50 * dirFlux[2];

          if(store_contact_data_)
          {
              conduction_contact_area_[i] += contactArea;
              n_conduction_contacts_[i] += 1.;
          }
          if (newton_pair || j < nlocal)
          {
            heatFlux[j] -= flux;
            directionalHeatFlux[j][0] += 0.50 * dirFlux[0];
            directionalHeatFlux[j][1] += 0.50 * dirFlux[1];
            directionalHeatFlux[j][2] += 0.50 * dirFlux[2];

            if(store_contact_data_)
            {
                conduction_contact_area_[j] += contactArea;
                n_conduction_contacts_[j] += 1.;
            }
          }
        }

        if(cpl_flag && cpl) cpl->add_heat(i,j,flux);
      }
    }
  }

 //printf("time_conduction \n");
  if(newton_pair)
  {
    fix_heatFlux->do_reverse_comm();
    fix_directionalHeatFlux->do_reverse_comm();
    fix_conduction_contact_area_->do_reverse_comm();
    fix_n_conduction_contacts_->do_reverse_comm();
  }

  if(!cpl_flag && store_contact_data_)
  for(int i = 0; i < nlocal; i++)
  {
     if(n_conduction_contacts_[i] > 0.5)
        conduction_contact_area_[i] /= n_conduction_contacts_[i];
  }
}

/* ----------------------------------------------------------------------
   register and unregister callback to compute
------------------------------------------------------------------------- */

void FixHeatGranRad::register_compute_pair_local(ComputePairGranLocal *ptr)
{
   
   if(cpl != NULL)
      error->all(FLERR,"Fix heat/gran/conduction allows only one compute of type pair/local");
   cpl = ptr;
}

void FixHeatGranRad::unregister_compute_pair_local(ComputePairGranLocal *ptr)
{
   
   if(cpl != ptr)
       error->all(FLERR,"Illegal situation in FixHeatGranRad::unregister_compute_pair_local");
   cpl = NULL;
}
