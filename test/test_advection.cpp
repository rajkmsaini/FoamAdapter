// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 NeoFOAM authors

#define CATCH_CONFIG_RUNNER // Define this before including catch.hpp to create
                            // a custom main

#include "NeoFOAM/dsl/expression.hpp"
#include "NeoFOAM/dsl/solver.hpp"
#include "NeoFOAM/dsl/ddt.hpp"
#include "FoamAdapter/readers/foamDictionary.hpp"

#include "NeoFOAM/dsl/implicit.hpp"
#include "NeoFOAM/dsl/explicit.hpp"


#include "FoamAdapter/FoamAdapter.hpp"
#include "FoamAdapter/readers/foamDictionary.hpp"

#define namespaceFoam
#include "fvCFD.H"

#include "common.hpp"


using Foam::Info;
using Foam::endl;
using Foam::nl;
namespace fvc = Foam::fvc;
namespace fvm = Foam::fvm;

namespace dsl = NeoFOAM::dsl;
namespace fvcc = NeoFOAM::finiteVolume::cellCentred;

extern Foam::Time* timePtr; // A single time object


void initFields(Foam::volScalarField& T, Foam::volVectorField& U, Foam::surfaceScalarField& phi)
{

    const Foam::fvMesh& mesh = T.mesh();


    Foam::scalar spread = 0.05;
    Foam::scalar pi = Foam::constant::mathematical::pi;
    forAll(U, celli)
    {
        // initialize U
        Foam::scalar x = mesh.C()[celli].x();
        Foam::scalar y = mesh.C()[celli].y();

        U[celli].x() = -Foam::sin(2.0 * pi * y) * Foam::pow(Foam::sin(pi * x), 2.0);
        U[celli].y() = Foam::sin(2.0 * pi * x) * Foam::pow(Foam::sin(pi * y), 2.0);
        U[celli].z() = 0.0;

        // initialize T
        T[celli] = std::exp(
            -0.5
            * (std::pow((mesh.C()[celli].x() - 0.5) / spread, 2.0)
               + std::pow((mesh.C()[celli].y() - 0.75) / spread, 2.0))
        );
    }
    T.correctBoundaryConditions();

    phi = Foam::linearInterpolate(U) & mesh.Sf();
}

TEST_CASE("Advection Equation")
{
    Foam::Time& runTime = *timePtr;

    NeoFOAM::Database db;
    fvcc::FieldCollection& fieldCollection = fvcc::FieldCollection::instance(db, "fieldCollection");

    NeoFOAM::Executor exec = GENERATE(
        NeoFOAM::Executor(NeoFOAM::SerialExecutor {}),
        NeoFOAM::Executor(NeoFOAM::CPUExecutor {}),
        NeoFOAM::Executor(NeoFOAM::GPUExecutor {})
    );

    std::string execName = std::visit([](auto e) { return e.name(); }, exec);

    std::string timeIntegration = GENERATE(std::string("forwardEuler"), std::string("Runge-Kutta"));

    SECTION("Scalar advection with " + execName + " and " + timeIntegration)
    {
        // create mesh
        Foam::scalar startTime = 0.0;
        Foam::label startTimeIndex = 0;
        runTime.setTime(startTime, startTimeIndex);

        std::unique_ptr<Foam::MeshAdapter> meshAdapterPtr = Foam::createMesh(exec, runTime);
        Foam::MeshAdapter& mesh = *meshAdapterPtr;

        NeoFOAM::Dictionary controlDict = Foam::readFoamDictionary(runTime.controlDict());
        NeoFOAM::Dictionary fvSchemesDict = Foam::readFoamDictionary(mesh.schemesDict());
        fvSchemesDict.get<NeoFOAM::Dictionary>("ddtSchemes").insert("type", timeIntegration);
        NeoFOAM::Dictionary fvSolutionDict = Foam::readFoamDictionary(mesh.solutionDict());

        NeoFOAM::UnstructuredMesh& nfMesh = mesh.nfMesh();

        Info << "Reading fields " << endl;

        Foam::volScalarField T(
            Foam::IOobject(
                "T",
                runTime.timeName(),
                mesh,
                Foam::IOobject::MUST_READ,
                Foam::IOobject::AUTO_WRITE
            ),
            mesh
        );

        Foam::volVectorField U(
            Foam::IOobject(
                "U",
                runTime.timeName(),
                mesh,
                Foam::IOobject::MUST_READ,
                Foam::IOobject::AUTO_WRITE
            ),
            mesh
        );

        Foam::surfaceScalarField phi("phi", Foam::linearInterpolate(U) & mesh.Sf());

        initFields(T, U, phi);

        // Copies of initial U and phi for use when flow is periodic
        Foam::surfaceScalarField phi0 = phi;
        Foam::volVectorField U0 = U;

        Info << "creating NeoFOAM fields" << endl;
        fvcc::VolumeField<NeoFOAM::scalar>& nfT =
            fieldCollection.registerField<fvcc::VolumeField<NeoFOAM::scalar>>(
                Foam::CreateFromFoamField<Foam::volScalarField> {
                    .exec = exec,
                    .nfMesh = nfMesh,
                    .foamField = T,
                    .name = "nfT"
                }
            );
        auto nfPhi0 = Foam::constructSurfaceField(exec, nfMesh, phi0);
        auto nfPhi = Foam::constructSurfaceField(exec, nfMesh, phi);

        Foam::scalar endTime = controlDict.get<Foam::scalar>("endTime");

        while (runTime.run())
        {
            Foam::scalar t = runTime.time().value();
            Foam::scalar dt = runTime.deltaT().value();

            Foam::scalar pi = Foam::constant::mathematical::pi;
            U = U0 * Foam::cos(pi * (t + 0.5 * dt) / endTime);
            phi = phi0 * Foam::cos(pi * (t + 0.5 * dt) / endTime);

            nfPhi.internalField() =
                nfPhi0.internalField() * std::cos(pi * (t + 0.5 * dt) / endTime);

            runTime++;

            Info << "Time = " << runTime.timeName() << endl;

            // advance Foam fields in time
            {
                Foam::fvScalarMatrix TEqn(fvm::ddt(T) + fvc::div(phi, T));

                TEqn.solve();
            }

            // advance NeoFOAM fields in time
            {
                dsl::Expression eqnSys(dsl::imp::ddt(nfT) + dsl::exp::div(nfPhi, nfT));

                dsl::solve(eqnSys, nfT, t, dt, fvSchemesDict, fvSolutionDict);
            }

            // for debugging with paraview
            if (runTime.outputTime())
            {
                Info << "writing nfT fields" << endl;
                write(nfT.internalField(), mesh, "nfT_" + execName);
                T.write(); // for some reason T was not written
            }

            runTime.write();
            runTime.printExecutionTime(Info);
        }
        // compare(nfT, T, ApproxScalar(1e-15), false);


        Info << "End\n" << endl;
    }
}