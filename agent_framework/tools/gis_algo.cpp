// GIS 算法验证：多边形面积、交集、并集、差集
// 使用 Boost.Geometry，验证常见 GIS 空间运算
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <iostream>
#include <iomanip>
#include <vector>

namespace bg = boost::geometry;
using Point = bg::model::d2::point_xy<double>;
using Polygon = bg::model::polygon<Point>;
using MultiPolygon = bg::model::multi_polygon<Polygon>;

// 辅助：从坐标对列表构建多边形（自动闭合）
Polygon make_poly(const std::vector<std::pair<double,double>>& pts) {
    Polygon p;
    for (auto& [x, y] : pts) bg::append(p, Point(x, y));
    bg::correct(p);  // 确保正确方向/闭合
    return p;
}

void print_poly(const std::string& name, const Polygon& p) {
    std::cout << name << ": " << bg::wkt(p) << "\n";
    std::cout << "  面积 = " << bg::area(p) << "\n\n";
}

int main() {
    std::cout << std::fixed << std::setprecision(4);
    
    // ---- 1. 多边形面积计算 ----
    std::cout << "===== 1. 多边形面积计算 =====\n";
    Polygon square = make_poly({{0,0},{4,0},{4,4},{0,4}});
    print_poly("正方形(4x4)", square);
    
    Polygon tri = make_poly({{0,0},{4,0},{2,3}});
    print_poly("三角形(底4高3)", tri);
    
    Polygon lshape = make_poly({{0,0},{6,0},{6,2},{2,2},{2,6},{0,6}});
    print_poly("L形", lshape);
    
    Polygon pent = make_poly({{0,0},{4,0},{5,3},{2,5},{-1,3}});
    print_poly("五边形", pent);
    
    // ---- 2. 多边形求交 ----
    std::cout << "===== 2. 多边形交集 =====\n";
    Polygon a = make_poly({{0,0},{4,0},{4,4},{0,4}});
    Polygon b = make_poly({{2,2},{6,2},{6,6},{2,6}});
    MultiPolygon inter;
    bg::intersection(a, b, inter);
    std::cout << "A∩B: " << bg::wkt(inter) << "\n";
    std::cout << "  交集面积 = " << bg::area(inter) << " (期望 4.0)\n\n";
    
    // ---- 3. 多边形并集 ----
    std::cout << "===== 3. 多边形并集 =====\n";
    MultiPolygon uni;
    bg::union_(a, b, uni);
    std::cout << "A∪B: " << bg::wkt(uni) << "\n";
    std::cout << "  并集面积 = " << bg::area(uni) << " (期望 28.0)\n\n";
    
    // ---- 4. 多边形差集 ----
    std::cout << "===== 4. 多边形差集 =====\n";
    MultiPolygon diff;
    bg::difference(a, b, diff);
    std::cout << "A-B: " << bg::wkt(diff) << "\n";
    std::cout << "  差集面积 = " << bg::area(diff) << " (期望 12.0)\n\n";
    
    // ---- 5. 点在多边形内判断 ----
    std::cout << "===== 5. 点在多边形内判断 =====\n";
    Point inside(1, 1), outside(10, 10), onedge(2, 0);
    std::cout << "点(1,1) 在正方形内: " << (bg::within(inside, square) ? "是" : "否") << "\n";
    std::cout << "点(10,10) 在正方形内: " << (bg::within(outside, square) ? "是" : "否") << "\n";
    std::cout << "点(2,0) 在正方形内(边): " << (bg::within(onedge, square) ? "是" : "否") << "\n\n";
    
    // ---- 6. 几何中心 / 质心 ----
    std::cout << "===== 6. 质心计算 =====\n";
    Point centroid;
    bg::centroid(square, centroid);
    std::cout << "正方形质心: (" << bg::get<0>(centroid) << ", " << bg::get<1>(centroid) << ")\n";
    bg::centroid(tri, centroid);
    std::cout << "三角形质心: (" << bg::get<0>(centroid) << ", " << bg::get<1>(centroid) << ")\n";
    bg::centroid(lshape, centroid);
    std::cout << "L形质心: (" << bg::get<0>(centroid) << ", " << bg::get<1>(centroid) << ")\n\n";
    
    // ---- 7. 点间距离 ----
    std::cout << "===== 7. 点间距离 =====\n";
    Point p1(0,0), p2(3,4);
    std::cout << "距离(0,0)-(3,4) = " << bg::distance(p1, p2) << " (期望 5.0)\n\n";
    
    std::cout << "===== 全部算法验证完成 =====\n";
    return 0;
}
