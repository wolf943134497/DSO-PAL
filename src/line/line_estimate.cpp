#include "line_estimate.h"
#include "FullSystem/CoarseTracker.h"
#include "FullSystem/HessianBlocks.h"

#include <ceres/ceres.h>
#include <ceres/rotation.h>

#include <iostream>

using namespace std;
using namespace Eigen;


namespace dso_line{

bool line3d_to_image(
        const Eigen::Vector3f &line_x0, const Eigen::Vector3f &line_u, 
        Eigen::Vector2f &line2d_x0, Eigen::Vector2f &line2d_u, 
        const Eigen::Matrix3f &R, const Eigen::Vector3f &t, 
        const Matrix3f &K){
    
    Vector3f P[2];
    P[0] = line_x0;
    P[1] = line_x0 + line_u;
    for(int i=0; i<2; i++){
        P[i] = R * P[i] + t; 
        P[i] /= P[i][2]; 
        P[i] = K * P[i];
    }
    line2d_x0 = P[0].block<2, 1>(0, 0);
    P[1] = P[1] - P[0];
    line2d_u = P[1].block<2, 1>(0, 0);
    line2d_u.normalize();
    }

float line_estimate_g(const Eigen::MatrixXf &points, Eigen::Vector3f &x0_ret, Eigen::Vector3f &u_ret){

    float dist_thres = 0.01;

    // RANSAC 求解   
    int npts = points.rows();
    Vector3f best_x0, best_u;
    int max_inlier = -1;

    for(int i=0; i<1000; i++){
        int r1 = rand() % npts;
        int r2 = rand() % npts;
        while(r2 == r1){
            r2 = rand() % npts;
        }

        // line model (x0+u)
        Vector3f x0 = points.row(r1); 
        Vector3f u = Vector3f(points.row(r2)) - Vector3f(points.row(r1));
        u.normalize();

        int inliner = 0;
        for(int j=0; j<npts; j++){
            Vector3f p = points.row(j);
            Vector3f distv = (x0 - p) + u.dot(p - x0) * u;
            float dist = distv.norm();
            if(dist < dist_thres){
                inliner ++ ;
            }
        }
        if(inliner > max_inlier){
            printf("*");
            max_inlier = inliner;
            best_x0 = x0;
            best_u = u;
        }
        // debug info
        // printf("(%2d) inline:%d/%d ", i, inliner, npts);
        // cout << x0.transpose() << "\t" << u.transpose() << endl;
    }

    x0_ret = best_x0;
    u_ret = best_u;
    return float(max_inlier)/npts;

    // Ceres NLSQ 稍微有点麻烦,比较难表达出残差项 
    // Problem problem;
    // CostFunction* cost_function = new AutoDiffCostFunction<LineResidual, 1, 1>(new LineResidual);
    // double x = 5;
    // problem.AddResidualBlock(cost_function, NULL, &x);
    // Solver::Options options;
    // options.linear_solver_type = DENSE_QR;
    // options.minimizer_progress_to_stdout = true;
    // Solver::Summary summary;
    // Solve(options, &problem, &summary);
    // cout << summary.BriefReport() << endl;
}

struct LineReprojectError{
public:
    // +直线全部用两点表示,不要用点+方向

    LineReprojectError(float gx, float gy, float dist, float l_p0[3], float l_p1[3], float camera[4])
        :gx_(gx), gy_(gy), dist_(dist){
            for(int i=0; i<3; i++){
                l_p0_[i] = l_p0[i];
                l_p1_[i] = l_p1[i];
            }
            for(int i=0; i<4; i++){
                camera_[i] = camera[i];
            }
        };

    template<typename T> 
    bool operator()(const T* const pose, T* residuals) const {
        T p0[3], p1[3];
        T l_p0T[3] = {T(l_p0_[0]), T(l_p0_[1]), T(l_p0_[2]) };
        T l_p1T[3] = {T(l_p1_[0]), T(l_p1_[1]), T(l_p1_[2]) };

        ceres::AngleAxisRotatePoint(pose, l_p0T, p0);
        ceres::AngleAxisRotatePoint(pose, l_p1T, p1);

        for(int i=0; i<3; i++){
            p0[i]+= pose[3+i];  
            p1[i]+= pose[3+i];
        }
        T lx = p1[0]/p1[2] - p0[0]/p0[2];
        T ly = p1[1]/p1[2] - p0[1]/p0[2];
        T l_norm = ceres::sqrt(lx*lx + ly*ly);
        lx /= l_norm; ly /= l_norm; 
        
        // 计算直线角度和像素梯度之差
        residuals[0] = acos(lx*gx_+ly*gy_)/dist_;
        return true;
    }

    static ceres::CostFunction *Create(
            float gx, float gy, float dist, 
            const Eigen::Vector3f &l_x0, const Eigen::Vector3f &l_u,
            const Eigen::Matrix3f &K){

        // double l_p0[3], l_p1[3], 
        float camera[4] = {K(0, 0), K(1, 1), K(0, 2), K(1, 2)};
        Eigen::Vector3f l_p0 = l_x0;
        Eigen::Vector3f l_p1 = l_p0 + l_u;
        
        // TODO: x0+u = p0;
        return (
            new ceres::AutoDiffCostFunction<LineReprojectError, 1, 6>(
                new LineReprojectError(gx, gy, dist, l_p0.data(), l_p1.data(), camera)));
    }

   private:
    const double gx_;
    const double gy_;
    const double dist_;
    double l_p0_[3];
    double l_p1_[3];
    double camera_[4];  // fx, fy, cx, cy
};
}

namespace dso{

	void CoarseTracker::testLine(const SE3 &refToNew){
        using namespace ceres;
        using namespace dso_line; 

        google::InitGoogleLogging("ceres_tttttttest");

        int lvl = 0;
        int nl = pc_n[lvl];
        int wl = w[lvl];
        int hl = h[lvl];
        int line_num = 1; //lastRef->line_u.size();
        Mat33f R = refToNew.rotationMatrix().cast<float>();
        Vec3f t = refToNew.translation().cast<float>();
        Mat33f K; K << fx[lvl], 0, cx[lvl], 0, fy[lvl], cy[lvl], 0, 0, 1;
        Vec3f* newImg = newFrame->dIp[lvl];

        Problem problem;
        double x_init = 0.0;
        double x_val = x_init;

        for(int i = 0; i<line_num; i++){
            Vec2f line2d_x0, line2d_u;
            print(输出一些调试信息确认这些代码正确);
            line3d_to_image(lastRef->line_x0[i], lastRef->line_u[i], line2d_x0, line2d_u, R, t, K);
            // 尝试把直线附近的点加入到residualblock中
            line2d_u = line2d_u / line2d_u[1];
            line2d_x0 = line2d_x0 + (0 - line2d_x0[1]) * line2d_u;
            for (int row = 3; row < hl - 3; row++) {
                Vec2f px = line2d_x0 + row * line2d_u;
                float px_col = px[0];
                if(px_col < 3 && px_col >= wl-3){
                    continue;
                }

                // 添加hit点
                Vec3f hit_img = getInterpolatedElement33(newImg, px_col, row, wl);
                CostFunction* cost_function = LineReprojectError::Create(
                        hit_img[1], hit_img[2], 0.5, 
                        lastRef->line_x0[i], lastRef->line_u[i], K);
                problem.AddResidualBlock(cost_function, NULL, &x_val);

                // 添加周围点
                int px_col_int = int(px_col);
                for(int j=-1; j<=1; j++){
                    if( j != 0){
                        int dist = abs(j);
                        Vec3f &px_img = newImg[row*wl+px_col_int + j];
                        CostFunction* cost_fun = LineReprojectError::Create(
                            px_img[1], px_img[2], dist, 
                            lastRef->line_x0[i], lastRef->line_u[i], K);
                        problem.AddResidualBlock(cost_fun, NULL, &x_val);
                    }
                }
            }
        }

        Solver::Options options;
        options.linear_solver_type = DENSE_QR;
        options.minimizer_progress_to_stdout = true;
        Solver::Summary summary;

        ceres::Solve(options, &problem, &summary);

        std::cout << summary.BriefReport() << "\n";
        std::cout << "x : " << x_init << " -> " << x_val << "\n"; 
        exit(0);
    }
}