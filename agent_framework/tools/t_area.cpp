#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <iostream>
#include <iomanip>
namespace bg=boost::geometry;
using P=bg::model::d2::point_xy<double>;
using Poly=bg::model::polygon<P>;
namespace boost{void throw_exception(std::exception const&){std::abort();}}
int main(){
    Poly A,B;
    bg::append(A,P(0,0));bg::append(A,P(4,0));bg::append(A,P(4,4));bg::append(A,P(0,4));bg::correct(A);
    bg::append(B,P(2,2));bg::append(B,P(6,2));bg::append(B,P(6,6));bg::append(B,P(2,6));bg::correct(B);
    std::cout<<std::fixed<<std::setprecision(4);
    std::cout<<"A面积="<<bg::area(A)<<"\n";
    std::cout<<"B面积="<<bg::area(B)<<"\n";
    std::cout<<"A周长="<<bg::perimeter(A)<<"\n";
    std::cout<<"A∩B="<<bg::intersects(A,B)<<"\n";
    std::cout<<"A disjoint B="<<bg::disjoint(A,B)<<"\n";
    Poly C;bg::append(C,P(10,10));bg::append(C,P(12,10));bg::append(C,P(12,12));bg::append(C,P(10,12));bg::correct(C);
    std::cout<<"A disjoint C="<<bg::disjoint(A,C)<<"\n";
    std::cout<<"A含(1,1)="<<bg::covered_by(P(1,1),A)<<"\n";
    std::cout<<"(1,1)within A="<<bg::within(P(1,1),A)<<"\n";
    std::cout<<"(5,5)within A="<<bg::within(P(5,5),A)<<"\n";
    std::cout<<"(2,0)within A="<<bg::within(P(2,0),A)<<"\n";
    std::cout<<"(2,0)covered A="<<bg::covered_by(P(2,0),A)<<"\n";
    P c;bg::centroid(A,c);
    std::cout<<"质心x="<<bg::get<0>(c)<<" y="<<bg::get<1>(c)<<"\n";
    std::cout<<"点距="<<bg::distance(P(0,0),P(3,4))<<"\n";
    return 0;
}
