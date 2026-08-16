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
Polygon mk(const std::vector<std::pair<double,double>>& p){
    Polygon q; for(auto&r:p) bg::append(q,Point(r.first,r.second)); bg::correct(q); return q;}
void pr(const std::string& l,double v,double e){std::cout<<std::left<<std::setw(24)<<l<<" = "<<std::setw(10)<<v<<" (期望 "<<e<<") "<<(std::abs(v-e)<1e-4?"✓":"✗ FAIL")<<"\n";}
void prb(const std::string& l,bool v,bool e){std::cout<<std::left<<std::setw(24)<<l<<" = "<<std::setw(10)<<(v?"是":"否")<<" (期望 "<<(e?"是":"否")<<") "<<(v==e?"✓":"✗ FAIL")<<"\n";}
int main(){
    std::cout<<std::fixed<<std::setprecision(4)<<"========== 布尔运算/关系 ==========\n";
    Polygon A=mk({{0,0},{4,0},{4,4},{0,4}}), B=mk({{2,2},{6,2},{6,6},{2,6}});
    MultiPolygon R;
    bg::intersection(A,B,R); pr("交集面积",bg::area(R),4.0);
    bg::union_(A,B,R); pr("并集面积",bg::area(R),28.0);
    bg::difference(A,B,R); pr("差集A-B",bg::area(R),12.0);
    bg::difference(B,A,R); pr("差集B-A",bg::area(R),12.0);
    bg::sym_difference(A,B,R); pr("对称差",bg::area(R),24.0);
    prb("A∩B",bg::intersects(A,B),true);
    prb("A含(1,1)",bg::covered_by(Point(1,1),A),true);
    prb("(1,1)within A",bg::within(Point(1,1),A),true);
    prb("(5,5)within A",bg::within(Point(5,5),A),false);
    prb("(2,0)within A边界",bg::within(Point(2,0),A),false);
    prb("(2,0)covered A",bg::covered_by(Point(2,0),A),true);
    prb("A disjoint B",bg::disjoint(A,B),false);
    Polygon C=mk({{10,10},{12,10},{12,12},{10,12}});
    prb("A disjoint C",bg::disjoint(A,C),true);
    return 0;
}
