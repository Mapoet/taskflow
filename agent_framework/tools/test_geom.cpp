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
namespace boost { void throw_exception(std::exception const&) { std::abort(); } }

Polygon mkpoly(const std::vector<std::pair<double,double>>& pts) {
    Polygon p; for (auto& pr : pts) bg::append(p, Point(pr.first, pr.second));
    bg::correct(p); return p;
}
void pr(const std::string& l, double v, double e) {
    std::cout << std::left << std::setw(28) << l << " = " << std::setw(10) << v
              << " (期望 " << e << ") " << (std::abs(v-e)<1e-4?"✓":"✗ FAIL") << "\n";
}
void prb(const std::string& l, bool v, bool e) {
    std::cout << std::left << std::setw(28) << l << " = " << std::setw(10) << (v?"是":"否")
              << " (期望 " << (e?"是":"否") << ") " << (v==e?"✓":"✗ FAIL") << "\n";
}

int main() {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "========== 核心 GIS 算法验证 ==========\n\n";
    Polygon A = mkpoly({{0,0},{4,0},{4,4},{0,4}});
    Polygon B = mkpoly({{2,2},{6,2},{6,6},{2,6}});
    MultiPolygon R;
    bg::intersection(A,B,R); pr("交集面积", bg::area(R), 4.0);
    bg::union_(A,B,R);       pr("并集面积", bg::area(R), 28.0);
    bg::difference(A,B,R);   pr("差集 A-B", bg::area(R), 12.0);
    bg::difference(B,A,R);   pr("差集 B-A", bg::area(R), 12.0);
    bg::sym_difference(A,B,R); pr("对称差", bg::area(R), 24.0);

    prb("A∩B", bg::intersects(A,B), true);
    prb("A含(1,1)", bg::covered_by(Point(1,1), A), true);
    prb("(1,1)within A", bg::within(Point(1,1), A), true);
    prb("(5,5)within A", bg::within(Point(5,5), A), false);
    prb("(2,0)within A边界", bg::within(Point(2,0), A), false);
    prb("(2,0)covered A", bg::covered_by(Point(2,0), A), true);
    prb("A disjoint B", bg::disjoint(A,B), false);
    Polygon C = mkpoly({{10,10},{12,10},{12,12},{10,12}});
    prb("A disjoint C", bg::disjoint(A,C), true);

    Point c; bg::centroid(A,c);
    pr("正方形质心x", bg::get<0>(c), 2.0); pr("正方形质心y", bg::get<1>(c), 2.0);
    Polygon tri = mkpoly({{0,0},{4,0},{2,3}}); bg::centroid(tri,c);
    pr("三角形质心x", bg::get<0>(c), 2.0); pr("三角形质心y", bg::get<1>(c), 1.0);

    pr("点距", bg::distance(Point(0,0),Point(3,4)), 5.0);
    pr("正方形周长", bg::perimeter(A), 16.0);
    pr("三角形面积", bg::area(tri), 6.0);
    pr("三角形周长", bg::perimeter(tri), 12.0);
    Polygon pent = mkpoly({{0,0},{4,0},{5,3},{2,5},{-1,3}});
    pr("五边形面积", bg::area(pent), 24.0);
    pr("五边形周长", bg::perimeter(pent), 18.4721);

    Polygon hull;
    bg::convex_hull(pent, hull);
    pr("凸包=自身", bg::area(hull), 24.0);
    std::vector<Point> pts={Point(0,0),Point(4,0),Point(4,4),Point(0,4),Point(2,2)};
    bg::model::multi_point<Point> mpts;
    for (auto&p:pts) bg::append(mpts,p);
    bg::convex_hull(mpts, hull);
    pr("5点凸包", bg::area(hull), 16.0);

    Polygon zigzag = mkpoly({{0,0},{1,0.1},{2,-0.1},{3,0.1},{4,0},{4,4},{0,4}});
    Polygon simp; bg::simplify(zigzag,simp,0.5);
    std::cout << "  简化后点数: " << bg::num_points(simp) << " (原 " << bg::num_points(zigzag) << ")\n";
    std::cout << "========== 完成 ==========\n";
    return 0;
}
