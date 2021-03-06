#define TINYMESH_API_EXPORT
#include "smooth.h"

#include <algorithm>
#include <unordered_map>

#include <Eigen/SparseCore>
#include <Eigen/IterativeLinearSolvers>

using ScalarType = double;
using IndexType = int64_t;
using Triplet = Eigen::Triplet<ScalarType, IndexType>;
using SparseMatrix = Eigen::SparseMatrix<ScalarType>;

#include "core/openmp.h"
#include "polymesh/face.h"
#include "polymesh/vertex.h"
#include "polymesh/halfedge.h"

namespace tinymesh {

void laplace_smooth(Mesh &mesh, double epsilon, int iterations) {
    for (int it = 0; it < iterations; it++) {
        // Volonoi tessellation
        const int nv = mesh.num_vertices();
        std::vector<Vec3> centroids(nv);
        std::vector<Vec3> normals(nv);

        // Compute centroids and tangent planes
        omp_parallel_for(int i = 0; i < mesh.num_vertices(); i++) {
            Vertex *v = mesh.vertex(i);

            // Collect surrounding vertices
            Vec3 org = v->pos();
            std::vector<Vec3> pts;
            for (auto vit = v->v_begin(); vit != v->v_end(); ++vit) {
                pts.push_back(vit->pos());
            }

            // Compute centroids, tangents, and binormals
            Vec3 cent(0.0);
            Vec3 norm(0.0);
            for (int i = 0; i < pts.size(); i++) {
                const int j = (i + 1) % pts.size();
                Vec3 e1 = pts[i] - org;
                Vec3 e2 = pts[j] - org;
                Vec3 g = (org + pts[i] + pts[j]) / 3.0;

                cent += g;
                norm += cross(e1, e2);
            }

            cent /= pts.size();
            const double l = length(norm);

            if (l != 0.0) {
                centroids[i] = cent;
                normals[i] = norm / l;
            }
        }

        // Update vertex positions
        omp_parallel_for(int i = 0; i < mesh.num_vertices(); i++) {
            Vertex *v = mesh.vertex(i);
            if (v->isBoundary() || v->isStatic()) {
                continue;
            }

            if (length(normals[i]) != 0.0) {
                const Vec3 pt = v->pos();
                Vec3 e = centroids[i] - pt;
                e -= normals[i] * dot(e, normals[i]);
                Vec3 newPos = (1.0 - epsilon) * v->pos() + epsilon * (pt + e);
                v->setPos(newPos);
            }
        }
    }
}

void taubin_smooth(Mesh &mesh, double shrink, double inflate, int iterations) {
    for (int it = 0; it < iterations * 2; it++) {
        // Volonoi tessellation
        const int nv = mesh.num_vertices();
        std::vector<Vec3> centroids(nv);
        std::vector<Vec3> normals(nv);

        // Compute centroids and tangent planes
        omp_parallel_for(int i = 0; i < mesh.num_vertices(); i++) {
            Vertex *v = mesh.vertex(i);

            // Collect surrounding vertices
            Vec3 org = v->pos();
            std::vector<Vec3> pts;
            for (auto vit = v->v_begin(); vit != v->v_end(); ++vit) {
                pts.push_back(vit->pos());
            }

            // Compute centroids, tangents, and binormals
            Vec3 cent(0.0);
            Vec3 norm(0.0);
            for (int i = 0; i < pts.size(); i++) {
                const int j = (i + 1) % pts.size();
                Vec3 e1 = pts[i] - org;
                Vec3 e2 = pts[j] - org;
                Vec3 g = (org + pts[i] + pts[j]) / 3.0;

                cent += g;
                norm += cross(e1, e2);
            }

            cent /= pts.size();
            const double l = length(norm);

            if (l != 0.0) {
                centroids[i] = cent;
                normals[i] = norm / l;
            }
        }

        // Update vertex positions
        const double epsilon = it % 2 == 0 ? shrink : -inflate;
        omp_parallel_for(int i = 0; i < mesh.num_vertices(); i++) {
            Vertex *v = mesh.vertex(i);
            if (v->isBoundary() || v->isStatic()) {
                continue;
            }

            if (length(normals[i]) != 0.0) {
                const Vec3 pt = v->pos();
                Vec3 e = centroids[i] - pt;
                e -= normals[i] * dot(e, normals[i]);
                Vec3 newPos = (1.0 - epsilon) * v->pos() + epsilon * (pt + e);
                v->setPos(newPos);
            }
        }
    }
}

void implicit_fair(Mesh &mesh, double epsilon, int iterations) {
    /*
     * This method is based on the following paper. The normalized version in Sec.5.5 is used.
     * Desbrun et al. "Implicit Fairing of Irregular Meshes using Diffusion and Curvature Flow", 1999.
     */

    for (int it = 0; it < iterations; it++) {
        // Indexing vertices
        const int n_verts = mesh.num_vertices();
        std::unordered_map<Vertex *, int64_t> v2i;
        for (int i = 0; i < n_verts; i++) {
            v2i.insert(std::make_pair(mesh.vertex(i), i));
        }

        // Compute diffusion matrix
        Eigen::MatrixXd X(n_verts, 3);
        std::vector<Triplet> triplets;

        for (int i = 0; i < n_verts; i++) {
            Vertex *v = mesh.vertex(i);

            // Compute Volonoi area
            double A = 0.0;
            for (auto f_it = v->f_begin(); f_it != v->f_end(); ++f_it) {
                std::vector<Vec3> vs;
                for (auto v_it = f_it->v_begin(); v_it != f_it->v_end(); ++v_it) {
                    vs.push_back(v_it->pos());
                }

                Assertion(vs.size() == 3, "Non-triangle face is detected!");

                A += 0.5 * length(cross(vs[1] - vs[0], vs[2] - vs[0])) / 6.0;
            }

            // Compute weights
            double sumW = 0.0;
            std::vector<Triplet> tripletsInColumn;
            for (auto he_it = v->ohe_begin(); he_it != v->ohe_end(); ++he_it) {
                Halfedge *ohe = he_it.ptr();
                Halfedge *ihe = ohe->rev();

                Vertex *xa = ohe->next()->dst();
                Vertex *xb = ohe->dst();
                Vertex *xc = ihe->dst();
                Vertex *xd = ihe->next()->dst();

                const Vec3 va = xa->pos();
                const Vec3 vb = xb->pos();
                const Vec3 vc = xc->pos();
                const Vec3 vd = xd->pos();

                const Vec3 e_ab = vb - va;
                const Vec3 e_ac = vc - va;
                const double cot_a = dot(e_ab, e_ac) / (length(cross(e_ab, e_ac)) + 1.0e-8);

                const Vec3 e_db = vb - vd;
                const Vec3 e_dc = vc - vd;
                const double cot_d = dot(e_db, e_dc) / (length(cross(e_db, e_dc)) + 1.0e-8);

                const double W = (cot_a + cot_d);
                tripletsInColumn.emplace_back(i, xb->index(), W);
                if (std::isnan(W) || std::isinf(W)) {
                    Warn("NaN of inf matrix entry is detedted!");
                }
                sumW += W;
            }

            for (const auto& t : tripletsInColumn) {
                triplets.emplace_back(t.row(), t.col(), t.value() / sumW);
            }
            triplets.emplace_back(i, i, -1.0);

            X(i, 0) = v->pos()[0];
            X(i, 1) = v->pos()[1];
            X(i, 2) = v->pos()[2];
        }

        // Solve sparse linear system
        SparseMatrix K(n_verts, n_verts);
        K.setFromTriplets(triplets.begin(), triplets.end());

        SparseMatrix I(n_verts, n_verts);
        I.setIdentity();

        SparseMatrix A = I - epsilon * K;

        Eigen::BiCGSTAB<SparseMatrix> cg;
        cg.setTolerance(1.0e-6);
        cg.setMaxIterations(50);
        cg.compute(A);

        Eigen::MatrixXd Xnext(n_verts, 3);
        Xnext = cg.solve(X);

        for (int i = 0; i < n_verts; i++) {
            const Vec3 newpos = Vec3(Xnext(i, 0), Xnext(i, 1), Xnext(i, 2));
            mesh.vertex(i)->setPos(newpos);
        }
    }
}

}  // namespace tinymesh