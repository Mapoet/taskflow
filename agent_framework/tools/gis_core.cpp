// GIS 核心算法验证：交集、并集、差集、within、质心、距离
// 用最小化模板的方式编译
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

Polygon make_poly(const std::vector<std::pair<double,double>>& pts) {
    Polygon p;
    for (auto& [x, y] : pts) bg::append(p, Point(x, y));
    bg::correct(p);
    return p;
}

int main() {
    std::cout << std::fixed << std::setprecision(4);

    Polygon a = make_poly({{0,0},{4,0},{4,4},{0,4}});
    Polygon b = make_poly({{2,2},{6,2},{6,6},{2,6}});

    MultiPolygon inter;
    bg::intersection(a, b, inter);
    std::cout << "交集 A∩B: " << bg::wkt(inter) << "\n";
    std::cout << "  交集面积 = " << bg::area(inter) << " (期望 4.0)\n\n";

    MultiPolygon uni;
    bg::union_(a, b, uni);
    std::cout << "并集 A∪B: " << bg::wkt(uni) << "\n";
    std::cout << "  并集面积 = " << bg::area(uni) << " (期望 28.0)\n\n";

    MultiPolygon diff;
    bg::difference(a, b, diff);
    std::cout << "差集 A-B: " << bg::wkt(diff) << "\n";
    std::cout << "  差集面积 = " << bg::area(diff) << " (期望 12.0)\n\n";

    std::cout << "点(1,1) 在正方形内: " << (bg::within(Point(1,1), a) ? "是" : "否") << "\n";
    std::cout << "点(10,10) 在正方形内: " << (bg::within(Point(10,10), a) ? "是" : "否") << "\n";
    std::cout << "点(2,0) 在正方形内(边界): " << (bg::within(Point(2,0), a) ? "是" : "否") << "\n\n";

    Point c;
    bg::centroid(a, c);
    std::cout << "正方形质心: (" << bg::get<0>(c) << ", " << bg::get<1>(c) << ") (期望 2,2)\n";
    Polygon tri = make_poly({{0,0},{4,0},{2,3}});
    bg::centroid(tri, c);
    std::cout << "三角形质心: (" << bg::get<0>(c) << ", " << bg::get<1>(c) << ") (期望 2,1)\n\n";

    std::cout << "距离(0,0)-(3,4) = " << bg::distance(Point(0,0), Point(3,4)) << " (期望 5.0)\n";
    std::cout << "正方形周长 = " << bg::perimeter(a) << " (期望 16.0)\n";
    std::cout << "三角形周长 = " << bg::perimeter(tri) << " (期望 12.0)\n\n";

    Polygon pent = make_poly({{0,0},{4,0},{5,3},{2,5},{-1,3}});
    std::cout << "五边形面积 = " << bg::area(pent) << "\n";
    std::cout << "五边形周长 = " << bg::perimeter(pent) << "\n\n";

    std::cout << "===== 核心算法验证完成 =====\n";
    return 0;
}
