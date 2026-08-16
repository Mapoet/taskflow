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
using Ring = bg::model::ring<Point>;

namespace boost {
void throw_exception(std::exception const&) { std::abort(); }
}

Polygon mkpoly(const std::vector<std::pair<double,double>>& pts) {
    Polygon p;
    for (auto& pr : pts) bg::append(p, Point(pr.first, pr.second));
    bg::correct(p);
    return p;
}
void pr(const std::string& label, double val, double exp) {
    std::cout << std::left << std::setw(28) << label << " = " << std::setw(10) << val
              << " (期望 " << exp << ") " << (std::abs(val-exp) < 1e-4 ? "✓" : "✗ FAIL") << "\n";
}
void prb(const std::string& label, bool val, bool exp) {
    std::cout << std::left << std::setw(28) << label << " = " << std::setw(10) << (val?"是":"否")
              << " (期望 " << (exp?"是":"否") << ") " << (val==exp?"✓":"✗ FAIL") << "\n";
}

int main() {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "========== 核心 GIS 算法验证 ==========\n\n";

    // 1. 交集/并集/差集
    Polygon A = mkpoly({{0,0},{4,0},{4,4},{0,4}});   // 16
    Polygon B = mkpoly({{2,2},{6,2},{6,6},{2,6}});   // 16
    MultiPolygon R;
    bg::intersection(A,B,R); pr("交集面积", bg::area(R), 4.0);
    bg::union_(A,B,R);       pr("并集面积", bg::area(R), 28.0);
    bg::difference(A,B,R);   pr("差集 A-B 面积", bg::area(R), 12.0);
    bg::difference(B,A,R);   pr("差集 B-A 面积", bg::area(R), 12.0);
    bg::sym_difference(A,B,R); pr("对称差面积", bg::area(R), 24.0);

    // 2. 相交/包含/within/disjoint
    prb("A 与 B 相交", bg::intersects(A,B), true);
    prb("A 包含点(1,1)", bg::covered_by(Point(1,1), A), true);
    prb("点(1,1) within A", bg::within(Point(1,1), A), true);
    prb("点(5,5) within A", bg::within(Point(5,5), A), false);
    prb("点(2,0) within A(边界)", bg::within(Point(2,0), A), false);
    prb("点(2,0) covered_by A", bg::covered_by(Point(2,0), A), true);
    prb("A 与 B disjoint", bg::disjoint(A,B), false);
    Polygon C = mkpoly({{10,10},{12,10},{12,12},{10,12}});
    prb("A 与 C disjoint", bg::disjoint(A,C), true);

    // 3. 质心
    Point c;
    bg::centroid(A, c); pr("正方形质心 x", bg::get<0>(c), 2.0);
    pr("正方形质心 y", bg::get<1>(c), 2.0);
    Polygon tri = mkpoly({{0,0},{4,0},{2,3}});
    bg::centroid(tri, c); pr("三角形质心 x", bg::get<0>(c), 2.0);
    pr("三角形质心 y", bg::get<1>(c), 1.0);

    // 4. 距离/周长/面积
    pr("点距(0,0)-(3,4)", bg::distance(Point(0,0), Point(3,4)), 5.0);
    pr("正方形周长", bg::perimeter(A), 16.0);
    pr("三角形面积", bg::area(tri), 6.0);
    pr("三角形周长", bg::perimeter(tri), 12.0);
    Polygon pent = mkpoly({{0,0},{4,0},{5,3},{2,5},{-1,3}});
    pr("五边形面积", bg::area(pent), 24.0);
    pr("五边形周长", bg::perimeter(pent), 18.4721);

    // 5. 凸包
    Polygon hull;
    bg::convex_hull(pent, hull);
    pr("五边形凸包面积(应=自身)", bg::area(hull), 24.0);
    std::vector<Point> pts = {Point(0,0),Point(4,0),Point(4,4),Point(0,4),Point(2,2)};
    bg::model::multi_point<Point> mpts;
    for (auto& p : pts) bg::append(mpts, p);
    bg::convex_hull(mpts, hull);
    pr("5点凸包面积(应=16)", bg::area(hull), 16.0);

    // 6. 简化(道格拉斯-普克)
    Polygon zigzag = mkpoly({{0,0},{1,0.1},{2,-0.1},{3,0.1},{4,0},{4,4},{0,4}});
    Polygon simp;
    bg::simplify(zigzag, simp, 0.5);
    std::cout << "  简化后点数: " << bg::num_points(simp) << " (原 " << bg::num_points(zigzag) << ")\n";

    // 7. 缓冲
    Polygon sq = mkpoly({{0,0},{2,0},{2,2},{0,2}});
    MultiPolygon buf;
    bg::buffer(sq, buf, bg::strategy::buffer::distance_symmetric<double>(1.0),
               bg::strategy::buffer::side_straight(),
               bg::strategy::buffer::join_round(12),
               bg::strategy::buffer::end_round(12),
               bg::strategy::buffer::point_circle(12));
    std::cout << "  缓冲面积(2x2+1边距): " << bg::area(buf) << " (期望≈12.57 圆角)\n";

    // 8. 最近点
    std::cout << "\n========== 验证完成 ==========\n";
    return 0;
}
