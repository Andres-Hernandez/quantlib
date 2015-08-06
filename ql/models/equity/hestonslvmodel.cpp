/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2015 Johannes Goettker-Schnetmann
 Copyright (C) 2015 Klaus Spanderen

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/


#include <ql/timegrid.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/models/equity/hestonmodel.hpp>
#include <ql/models/equity/hestonslvmodel.hpp>
#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/integrals/discreteintegrals.hpp>
#include <ql/math/interpolations/bilinearinterpolation.hpp>
#include <ql/termstructures/volatility/equityfx/localvoltermstructure.hpp>
#include <ql/termstructures/volatility/equityfx/fixedlocalvolsurface.hpp>
#include <ql/methods/finitedifferences/meshers/predefined1dmesher.hpp>
#include <ql/methods/finitedifferences/solvers/fdmbackwardsolver.hpp>
#include <ql/methods/finitedifferences/operators/fdmlinearoplayout.hpp>
#include <ql/methods/finitedifferences/meshers/concentrating1dmesher.hpp>
#include <ql/methods/finitedifferences/meshers/fdmmeshercomposite.hpp>
#include <ql/methods/finitedifferences/utilities/fdmmesherintegral.hpp>
#include <ql/methods/finitedifferences/schemes/modifiedcraigsneydscheme.hpp>

#include <ql/experimental/finitedifferences/fdmhestonfwdop.hpp>
#include <ql/experimental/finitedifferences/localvolrndcalculator.hpp>
#include <ql/experimental/finitedifferences/squarerootprocessrndcalculator.hpp>

#include <boost/make_shared.hpp>
#include <boost/assign/std/vector.hpp>

#include <functional>
#include <iostream>

using namespace boost::assign;

namespace QuantLib {

    namespace {
        boost::shared_ptr<Fdm1dMesher> varianceMesher(
            const SquareRootProcessRNDCalculator& rnd, Time t, Size vGrid,
                Real v0, FdmSquareRootFwdOp::TransformationType trafoType) {
            switch (trafoType) {
                case FdmSquareRootFwdOp::Log:
                  {
                    std::vector<boost::tuple<Real, Real, bool> > cPoints;
                    const Real eps = 5e-4;
//                    const Real lowerBound = std::max(
//                            std::log(rnd.invcdf(eps, t)),
//                            std::log(0.0000025));
//                    const Real upperBound = std::log(rnd.invcdf(1-eps, t));
                    const Real lowerBound = std::log(0.0000025);
                    const Real upperBound = std::log(rnd.stationary_invcdf(1-eps));
                    const Real v0Center = std::log(v0);
                    const Real v0Density = t<0.5? 0.7 : 1.0;
                    const Real upperBoundDensity = 1.0;
                    const Real lowerBoundDensity = 0.1; // 0.1
                    cPoints += boost::make_tuple(lowerBound, lowerBoundDensity, false),
                              boost::make_tuple(v0Center, v0Density, true),
                              boost::make_tuple(upperBound, upperBoundDensity, false);
                    return boost::make_shared<Concentrating1dMesher>(
                        lowerBound, upperBound, vGrid, cPoints, 1e-8);
                  }
                break;
                case FdmSquareRootFwdOp::Plain:
                  {
                      const Real eps = 1e-4;
                      const Real lowerBound = rnd.invcdf(eps, t);
                      const Real upperBound = rnd.invcdf(1-eps, t);

                      std::vector<boost::tuple<Real, Real, bool> > cPoints;

                      const Real lowerBoundDensity = 0.1;
                      cPoints += boost::make_tuple(
                          lowerBound, lowerBoundDensity, false);

                      return boost::make_shared<Concentrating1dMesher>(
                          lowerBound, upperBound, vGrid, cPoints, 1e-8);
//                    upperBound = std::max(1.25*v0, rnd.stationary_invcdf(0.995));
//                    lowerBound = std::min(0.75*v0, rnd.stationary_invcdf(1e-5));
//
//                    const Real v0Center = v0;
//                    const Real v0Density = 0.01;
//                    const Real lowerBoundDensity = 0.05;
//                    cPoints += boost::make_tuple(lowerBound, lowerBoundDensity, false),
//                        boost:: make_tuple(v0Center, v0Density, true);
                  }
                break;
                default:
                    QL_FAIL("transformation type is not implemented");
            }
        }

        Disposable<Array> rescalePDF(
            const Array& p,
            const boost::shared_ptr<FdmMesherComposite>& mesher) {

            Array retVal = p/FdmMesherIntegral(
                mesher, DiscreteSimpsonIntegral()).integrate(p);

            return retVal;
        }


        template <class Interpolator>
        Disposable<Array> reshapePDF(
            const Array& p,
            const boost::shared_ptr<FdmMesherComposite>& oldMesher,
            const boost::shared_ptr<FdmMesherComposite>& newMesher,
            const Interpolator& interp = Interpolator()) {

            const boost::shared_ptr<FdmLinearOpLayout> oldLayout
                = oldMesher->layout();
            const boost::shared_ptr<FdmLinearOpLayout> newLayout
                = newMesher->layout();

            QL_REQUIRE(   oldLayout->size() == newLayout->size()
                       && oldLayout->size() == p.size(),
                       "inconsistent mesher or vector size given");

            Matrix m(oldLayout->dim()[1], oldLayout->dim()[0]);
            for (Size i=0; i < m.rows(); ++i) {
                std::copy(p.begin() + i*m.columns(),
                          p.begin() + (i+1)*m.columns(), m.row_begin(i));
            }
            const Interpolation2D interpol = interp.interpolate(
                oldMesher->getFdm1dMeshers()[0]->locations().begin(),
                oldMesher->getFdm1dMeshers()[0]->locations().end(),
                oldMesher->getFdm1dMeshers()[1]->locations().begin(),
                oldMesher->getFdm1dMeshers()[1]->locations().end(), m);

            const Real newXmin = *(newMesher->getFdm1dMeshers()[0]->locations().begin());
            const Real newXmax = *(newMesher->getFdm1dMeshers()[0]->locations().end()-1);
            const Real newVmin = *(newMesher->getFdm1dMeshers()[1]->locations().begin());
            const Real newVmax = *(newMesher->getFdm1dMeshers()[1]->locations().end()-1);

            Array pNew(p.size());
            const FdmLinearOpIterator endIter = newLayout->end();
            for (FdmLinearOpIterator iter = newLayout->begin();
                iter != endIter; ++iter) {
                const Real x = newMesher->location(iter, 0);
                const Real v = newMesher->location(iter, 1);

                if (   x > interpol.xMax() || x < interpol.xMin()
                    || v > interpol.yMax() || v < interpol.yMin() ) {
                    pNew[iter.index()] = 0;
//                    const Real xParam = std::max(interpol.xMin(), std::min(interpol.xMax(), x));
//                    const Real vParam = std::max(interpol.yMin(), std::min(interpol.yMax(), v));
//
//                    const Real xScale =
//                            (x > interpol.xMax() && newXmax > interpol.xMax())?
//                                    (newXmax-x)/(newXmax-interpol.xMax()) :
//                            (x < interpol.xMin() && interpol.xMin() < newXmin)?
//                                     (x-newXmin)/(interpol.xMin()-newXmin)
//                            :1.0;
//                    const Real vScale =
//                            (v > interpol.yMax() && newVmax > interpol.yMax())?
//                                    (newVmax-v)/(newVmax-interpol.yMax()) :
//                            (v < interpol.yMin() && interpol.yMin() < newVmin)?
//                                     (v-newVmin)/(interpol.yMin()-newVmin)
//                            :1.0;
//                    //std::cout << "xScale=" << xScale << ", vScale=" << vScale << std::endl;
//
////                    if (xScale*vScale >= 0.0) {
////                        std::cout << "xScale=" << xScale << ", vScale=" << vScale << std::endl;
////                        std::cout << "x=" << x << ", v=" << v << std::endl;
////                        std::cout << "xMax=" << interpol.xMax() << ", xMin=" << interpol.xMin() << std::endl;
////                        std::cout << "xMax=" << newXmax << ", xMin=" << newXmin << std::endl;
////                        std::cout << "vMax=" << interpol.yMax() << ", vMin=" << interpol.yMin() << std::endl;
////                        std::cout << "vMax=" << newVmax << ", vMin=" << newVmin << std::endl;
////                    }
//
//                    // expect flat extrapolation
//                    pNew[iter.index()] = interpol(xParam, vParam)*xScale*vScale;
////                    std::cout << x << ", " << v << " pNew=" << pNew[iter.index()] << std::endl;
                }
                else {
                    pNew[iter.index()] = interpol(x, v);
                }
            }

            return pNew;
        }
    }

    HestonSLVModel::HestonSLVModel(
        const Handle<LocalVolTermStructure>& localVol,
        const Handle<HestonModel>& hestonModel,
        const HestonSLVFokkerPlanckFdmParams& params,
        const std::vector<Date>& mandatoryDates)
        : localVol_(localVol),
          hestonModel_(hestonModel),
          params_(params),
          mandatoryDates_(mandatoryDates) {

        registerWith(hestonModel_);
        registerWith(localVol_);
    }

    void HestonSLVModel::update() {
        notifyObservers();
    }

    boost::shared_ptr<HestonProcess> HestonSLVModel::hestonProcess() const {
        return hestonModel_->process();
    }

    boost::shared_ptr<LocalVolTermStructure> HestonSLVModel::localVol() const {
        return localVol_.currentLink();
    }

    boost::shared_ptr<LocalVolTermStructure>
    HestonSLVModel::leverageFunction() const {
        calculate();

        return leverageFunction_;
    }

    void HestonSLVModel::performCalculations() const {
        const boost::shared_ptr<HestonProcess> hestonProcess
            = hestonModel_->process();
        const boost::shared_ptr<Quote> spot
            = hestonProcess->s0().currentLink();
        const boost::shared_ptr<YieldTermStructure> rTS
            = hestonProcess->riskFreeRate().currentLink();
        const boost::shared_ptr<YieldTermStructure> qTS
            = hestonProcess->dividendYield().currentLink();

        const Real v0    = hestonProcess->v0();
        const Real kappa = hestonProcess->kappa();
        const Real theta = hestonProcess->theta();
        const Real sigma = hestonProcess->sigma();
        const Real rho   = hestonProcess->rho();

        const Size xGrid = params_.xGrid;
        const Size vGrid = params_.vGrid;

        const DayCounter dc = rTS->dayCounter();
        const Date referenceDate = rTS->referenceDate();

        const Time T = dc.yearFraction(
            referenceDate, params_.finalCalibrationMaturity);

        QL_REQUIRE(referenceDate < params_.finalCalibrationMaturity,
            "reference date must be smaller than final calibration date");

        QL_REQUIRE(localVol_->maxTime() >= T,
            "final calibration maturity exceeds local volatility surface");

        // set-up exponential time step scheme
        const Time maxDt = 1.0/params_.tMaxStepsPerYear;
        const Time minDt = 1.0/params_.tMinStepsPerYear;

        Time t=0.0;
        std::vector<Time> times(1, t);
        times.reserve(T*params_.tMinStepsPerYear);
        while (t < T) {
            const Real decayFactor = std::exp(-params_.tStepNumberDecay*t);
            const Time dt = maxDt*decayFactor + minDt*(1.0-decayFactor);

            times.push_back(std::min(T, t+=dt));
        }

        for (Size i=0; i < mandatoryDates_.size(); ++i) {
            times.push_back(
                dc.yearFraction(referenceDate, mandatoryDates_[i]));
        }

        const boost::shared_ptr<TimeGrid> timeGrid(
            new TimeGrid(times.begin(), times.end()));

        // build 1d meshers
        const LocalVolRNDCalculator localVolRND(
            spot, rTS, qTS, localVol_.currentLink(),
            timeGrid, xGrid,
            params_.epsProbability,
            params_.undefinedlLocalVolOverwrite,
            params_.maxIntegrationIterations);

        std::cout << "eps for grid limits=" << params_.epsProbability << std::endl;

        const std::vector<Size> rescaleSteps
            = localVolRND.rescaleTimeSteps();

        const SquareRootProcessRNDCalculator squareRootRnd(
            v0, kappa, theta, sigma);

        const FdmSquareRootFwdOp::TransformationType trafoType
          = params_.trafoType;

        std::vector<boost::shared_ptr<Fdm1dMesher> > xMesher, vMesher;
        xMesher.reserve(timeGrid->size());
        vMesher.reserve(timeGrid->size());

//--------------------------------
//        const Real sEps = 1e-3;
//        const Real normInvEps = InverseCumulativeNormal()(1-sEps);
//        const Real x0 = std::log(spot->value());
//        const Time maturity = 2.0;
//        const Real localVol = localVol_->localVol(0.0, spot->value());
//
//        const Real sLowerBound = x0 - normInvEps*localVol*std::sqrt(maturity);
//        const Real sUpperBound = x0 + normInvEps*localVol*std::sqrt(maturity);
//
//        const boost::shared_ptr<Fdm1dMesher> spotMesher(
//            new Concentrating1dMesher(sLowerBound, sUpperBound, xGrid,
//                std::make_pair(x0, 0.1), true));
//--------------------------------

        xMesher.push_back(localVolRND.mesher(0.0));
        vMesher.push_back(boost::make_shared<Predefined1dMesher>(
            std::vector<Real>(vGrid, v0)));

        xMesher.push_back(localVolRND.mesher(timeGrid->at(1)));
        vMesher.push_back(
            varianceMesher(squareRootRnd, timeGrid->at(1), vGrid, v0, trafoType));

        for (Size i=2; i < timeGrid->size(); ++i) {
            xMesher.push_back(localVolRND.mesher(timeGrid->at(i)));
//            xMesher.push_back(spotMesher);

            const Size idx = std::distance(rescaleSteps.begin(),
                std::lower_bound(rescaleSteps.begin(),
                                 rescaleSteps.end(), i));

            if (i == rescaleSteps[idx-1]+1)
                vMesher.push_back(varianceMesher(squareRootRnd,
                    (idx < rescaleSteps.size())
                        ? timeGrid->at(rescaleSteps[idx])
                        : timeGrid->back(),
                    vGrid, v0, trafoType));
            else
                vMesher.push_back(vMesher.back());
        }

        // start probability distribution
        boost::shared_ptr<FdmMesherComposite> mesher
            = boost::make_shared<FdmMesherComposite>(
                xMesher.at(1), vMesher.at(1));

        const Volatility lv0
            = localVol_->localVol(0.0, spot->value())/std::sqrt(v0);

        boost::shared_ptr<Matrix> L(new Matrix(xGrid, timeGrid->size()));

        // just preliminarily put something other than zero into the matrix
        //std::fill(L->begin(),L->end(), 0.45);

        const Real l0 = lv0; ///std::sqrt(v0);
        std::fill(L->column_begin(0),L->column_end(0), l0);
        std::fill(L->column_begin(1),L->column_end(1), l0);

        std::cout << "initial vols: lv0: " << lv0 << ", l0: " << l0 << std::endl;

        // create strikes from meshers
        std::vector<boost::shared_ptr<std::vector<Real> > > vStrikes(
              timeGrid->size());

        for (Size i=0; i < timeGrid->size(); ++i) {
            vStrikes[i] = boost::make_shared<std::vector<Real> >(xGrid);
            std::transform(xMesher[i]->locations().begin(),
                           xMesher[i]->locations().end(),
                           vStrikes[i]->begin(),
                           std::ptr_fun<Real, Real>(std::exp));
        }

        boost::shared_ptr<FixedLocalVolSurface> leverageFct(
            new FixedLocalVolSurface(referenceDate, times, vStrikes, L, dc));

        boost::shared_ptr<FdmLinearOpComposite> hestonFwdOp(
            new FdmHestonFwdOp(mesher, hestonProcess,
                               trafoType, leverageFct));

        boost::shared_ptr<ModifiedCraigSneydScheme> mcg(
                new ModifiedCraigSneydScheme(
            FdmSchemeDesc::ModifiedCraigSneyd().theta,
            FdmSchemeDesc::ModifiedCraigSneyd().mu, hestonFwdOp));

        Array p = FdmHestonGreensFct(mesher, hestonProcess, trafoType, lv0)
            .get(timeGrid->at(1), params_.greensAlgorithm);

        // just a test for the reshape function at the moment
        // needs to implement the evolver of the PDF
        std::cout << "initial integral " <<
            FdmMesherIntegral(
                mesher, DiscreteSimpsonIntegral()).integrate(p)
                << std::endl;
        for (Size i = 2; i < times.size(); ++i) {
            const Time t = timeGrid->at(i);
            const Time dt = t - timeGrid->at(i-1);

            if (std::find(rescaleSteps.begin(), rescaleSteps.end(), i)
                != rescaleSteps.end()) {
                const boost::shared_ptr<FdmMesherComposite> newMesher(
                    new FdmMesherComposite(xMesher[i], vMesher[i]));

                p = reshapePDF<Bilinear>(p, mesher, newMesher);
                mesher = newMesher;

                std::cout << "reshape step " << i << " " <<
                    FdmMesherIntegral(
                        newMesher, DiscreteSimpsonIntegral()).integrate(p)
                        << std::endl;
            }
        }

        for (Size i=0; i < rescaleSteps.size(); ++i)
            std::cout << rescaleSteps[i] << " ";
        std::cout << std::endl;

//////////////////////////////////////
        // calculate leverage function
        //Array
        mesher = boost::make_shared<FdmMesherComposite>(
                xMesher.at(1), vMesher.at(1));
        p = FdmHestonGreensFct(mesher, hestonProcess, trafoType, lv0)
            .get(timeGrid->at(1), params_.greensAlgorithm);
        std::cout << "initial integral " <<
            FdmMesherIntegral(
                mesher, DiscreteSimpsonIntegral()).integrate(p)
                << std::endl;
        for (Size i=2; i < times.size(); ++i) {
            const Time t = timeGrid->at(i);
            const Time dt = t - timeGrid->at(i-1);

            if (std::find(rescaleSteps.begin(), rescaleSteps.end(), i)
                != rescaleSteps.end()) {
                std::cout << "pre reshape step " << i << " " <<
                    FdmMesherIntegral(
                        mesher, DiscreteSimpsonIntegral()).integrate(p)
                        << std::endl;
                const boost::shared_ptr<FdmMesherComposite> newMesher(
                    new FdmMesherComposite(xMesher[i], vMesher[i]));

                p = reshapePDF<Bilinear>(p, mesher, newMesher);
                mesher = newMesher;

                std::cout << "t=" << t << std::endl
                          << "x("
                          << mesher->locations(0).front()
                          << ", "
                          << mesher->locations(0).back()
                          << "), v("
                          << mesher->locations(1).front()
                          << ", "
                          << mesher->locations(1).back()
                          << ") xn="
                          << mesher->getFdm1dMeshers()[0]->locations().size()
                          << " vn="
                          << mesher->getFdm1dMeshers()[1]->locations().size()
                          << std::endl;
                std::cout << "x("
                          << std::exp(mesher->locations(0).front())
                          << ", "
                          << std::exp(mesher->locations(0).back())
                          << "), v("
                          << std::exp(mesher->locations(1).front())
                          << ", "
                          << std::exp(mesher->locations(1).back())
                          << ")"
                          << std::endl;

                std::cout << "reshape step " << i << " " <<
                    FdmMesherIntegral(
                        mesher, DiscreteSimpsonIntegral()).integrate(p)
                        << std::endl;
                //p = rescalePDF(p, mesher);
                hestonFwdOp = boost::shared_ptr<FdmLinearOpComposite>(
                                new FdmHestonFwdOp(mesher, hestonProcess,
                                               trafoType, leverageFct));
                mcg = boost::shared_ptr<ModifiedCraigSneydScheme>(
                        new ModifiedCraigSneydScheme(
                    FdmSchemeDesc::ModifiedCraigSneyd().theta,
                    FdmSchemeDesc::ModifiedCraigSneyd().mu, hestonFwdOp));
            }
            //std::cout << "reshape finished" << std::endl;

            Array pn = p;
            const Array x(Exp(
                    Array(
                      mesher->getFdm1dMeshers()[0]->locations().begin(),
                      mesher->getFdm1dMeshers()[0]->locations().end())));
            const Array v(
                    mesher->getFdm1dMeshers()[1]->locations().begin(),
                    mesher->getFdm1dMeshers()[1]->locations().end());

            // predictor corrector steps
            for (Size r=0; r < 3; ++r) {
                for (Size j=0; j < x.size(); ++j) {

                    Array pSlice(vGrid);
                    for (Size k=0; k < vGrid; ++k)
                        pSlice[k] = pn[j + k*xGrid];

                    const Real pInt = DiscreteSimpsonIntegral()(v, pSlice);

                    const Real vpInt = (trafoType == FdmSquareRootFwdOp::Log)
                      ? DiscreteSimpsonIntegral()(v, Exp(v)*pSlice)
                      : DiscreteSimpsonIntegral()(v, v*pSlice);

                    const Real scale = pInt/vpInt;
                    const Volatility localVol
                        = localVol_->localVol(t, x[j]);

                    const Real l = (scale >= 0.0)
                      ? localVol*std::sqrt(scale)
                      : 1.0;

                    (*L)[j][i] = std::min(5.0, std::max(0.01, l));

                    leverageFct->setInterpolation(Linear());
                }

// smoothing
                // TODO: Need to determine localvol at lower and upper bound!
                const Volatility localVol
                    = localVol_->localVol(t, x[x.size()/2]);
                const Volatility stdDev = localVol*std::sqrt(t);
                const Real xm
                    = std::log(spot->value()*qTS->discount(t)/rTS->discount(t))
                        -0.5*stdDev*stdDev;

                const Real normInvEps = InverseCumulativeNormal()(1-1e-4);

                const Real sLowerBound = std::max(
                    x.front(), std::exp(xm - normInvEps*stdDev));
                const Real sUpperBound = std::min(
                    x.back(), std::exp(xm + normInvEps*stdDev));

                const Real lowerL = leverageFct->localVol(t, sLowerBound);
                const Real upperL = leverageFct->localVol(t, sUpperBound);

                for (Size j=0; j < x.size(); ++j) {
                    if (x[j] < sLowerBound)
                        std::fill(L->row_begin(j)+i,
                          std::min(L->row_begin(j)+i+1, L->row_end(j)),
                          lowerL);
                    else if (x[j] > sUpperBound)
                        std::fill(L->row_begin(j)+i,
                          std::min(L->row_begin(j)+i+1, L->row_end(j)),
                          upperL);
                    else if ((*L)[j][i] == Null<Real>())
                        std::cout << t << " ouch" << std::endl;
                }
                leverageFct->setInterpolation(Linear());

                pn = p;
                mcg->setStep(dt);
                mcg->step(pn, t);
            }
            p = pn;

//          for (Size j=0; j < x.size(); ++j) {
//              std::cout << (*leverageFct)(t, x[j]) << " ";
//          }
//          std::cout << std::endl << std::endl;

        }
//////////////////////////////////////
        for (Size i = 0; i < 1 /*vStrikes.size()*/; i++) {
            std::cout << "Grid at time " << times[i] << " : " << std::endl;
            for (Size j = 0; j < vStrikes[i]->size(); j++) {
                std::cout << (*vStrikes[i])[j] << ", ";
            }
            std::cout << std::endl;
        }
        std::cout << std::endl;

        leverageFunction_ = leverageFct;
    }
}
