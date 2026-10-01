#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace Poseidon {
// Exact integral of max(head - linear triangle height, 0). Heights are sorted
// once; all arithmetic uses relative heights to avoid subtracting large cubics.
struct RainWaterTriangle {
    float low=0,middle=0,high=0;
    RainWaterTriangle()=default;
    RainWaterTriangle(float a,float b,float c) {
        std::array<float,3> h{a,b,c};std::sort(h.begin(),h.end());
        low=h[0];middle=h[1];high=h[2];
    }
    double Mean() const {return double(low)+(double(middle)-low+double(high)-low)/3;}
    double Volume(double head,double area) const {
        const double t=head-low,e=double(middle)-low,d=double(high)-low;
        if(t<=0)return 0;
        if(t>=d)return area*(t-(e+d)/3);
        if(t<e)return area*t*t*t/(3*e*d);
        // Integral from middle upwards. Stable also for low==middle; unlike
        // the equivalent high-tail expression, this has no large cancellation.
        const double u=t-e,s=d-e;
        return area*(e*e/(3*d)+e*u/d+u*u/d*(1-u/(3*s)));
    }
    double WetArea(double head,double area) const {
        const double t=head-low,e=double(middle)-low,d=double(high)-low;
        if(t<=0)return 0;
        if(t>=d)return area;
        if(t<e)return area*t*t/(e*d);
        const double u=t-e,s=d-e;
        return area*(e/d+u/d*(2-u/s));
    }
};

struct RainWaterCellGeometry {
private:
    // 00/10/01 and 10/11/01: exact Landscape anti-diagonal.
    float h00=0,h10=0,h01=0,h11=0;
    RainWaterTriangle first,second;
public:
    RainWaterCellGeometry()=default;
    RainWaterCellGeometry(float a,float b,float c,float d)
        :h00(a),h10(b),h01(c),h11(d),first(a,b,c),second(b,d,c) {}
    static RainWaterCellGeometry Flat(float h) {return {h,h,h,h};}
    float Corner00() const {return h00;}
    float Corner10() const {return h10;}
    float Corner01() const {return h01;}
    float Corner11() const {return h11;}
    bool Finite() const {return std::isfinite(h00)&&std::isfinite(h10)&&std::isfinite(h01)&&std::isfinite(h11);}
    double Minimum() const {return std::min(first.low,second.low);}
    double Maximum() const {return std::max(first.high,second.high);}
    double Mean() const {return (first.Mean()+second.Mean())*.5;}
    double Volume(double head,double area) const {
        return first.Volume(head,area*.5)+second.Volume(head,area*.5);
    }
    double WetArea(double head,double area) const {
        return first.WetArea(head,area*.5)+second.WetArea(head,area*.5);
    }
    // Area-averaged depth across an actual linear shared-face profile.
    static double FaceDepth(double head,double a,double b) {
        if(a>b)std::swap(a,b);
        if(head<=a)return 0;
        if(head>=b)return head-(a+b)*.5;
        const double wet=head-a;return wet*wet/(2*(b-a));
    }
    double Height(double x,double z) const {
        return x+z<=1?double(h00)+(h10-double(h00))*x+(h01-double(h00))*z:
            double(h11)+(h01-double(h11))*(1-x)+(h10-double(h11))*(1-z);
    }
    bool Head(double volume,double area,double& result,double hint=std::numeric_limits<double>::quiet_NaN()) const {
        if(!Finite()||!std::isfinite(volume)||volume<0||!std::isfinite(area)||area<=0)return false;
        const double low=Minimum(),high=Maximum();
        if(volume==0){result=low;return true;}
        if(low==high||volume>=Volume(high,area)) {
            result=Mean()+volume/area;return std::isfinite(result);
        }
        double lo=low,hi=high;
        double head=std::isfinite(hint)&&hint>lo&&hint<hi?hint:
            std::clamp(low+std::cbrt(volume/area*(high-low)*(high-low)*3),lo,hi);
        for(int iteration=0;iteration<12;++iteration) {
            const double v=Volume(head,area),error=v-volume;
            if(std::abs(error)<=volume*1e-11){result=head;return true;}
            if(error>0)hi=head;else lo=head;
            const double wet=WetArea(head,area);
            const double next=wet>0?head-error/wet:std::numeric_limits<double>::quiet_NaN();
            head=std::isfinite(next)&&next>lo&&next<hi?next:(lo+hi)*.5;
        }
        // Rare degenerate/near-dry fallback, still analytic curve evaluations;
        // never polygon clipping per owner per frame.
        for(int iteration=0;iteration<48;++iteration) {
            const double v=Volume(head,area);
            if(std::abs(v-volume)<=volume*1e-11||std::nextafter(lo,hi)>=hi){result=head;return true;}
            if(v>volume)hi=head;else lo=head;
            head=(lo+hi)*.5;
        }
        result=head;return true;
    }
};
// Conservative within-parent reconstruction: sixteen native triangle cells
// share an equilibrium head, not an equal depth on unrelated beds. This is a
// local sub-grid initialization, not recovery of an unknown historic shoreline.
inline bool RainWaterReconstruct(std::span<const RainWaterCellGeometry> geometry,
    double volume,double childArea,std::span<double> children) {
    if(geometry.empty()||children.size()!=geometry.size()||!std::isfinite(volume)||volume<0||
       !std::isfinite(childArea)||childArea<=0)return false;
    double low=std::numeric_limits<double>::infinity(),high=-low;
    for(const auto& g:geometry) {
        if(!g.Finite())return false;
        low=std::min(low,g.Minimum());high=std::max(high,g.Maximum());
    }
    if(volume==0){std::fill(children.begin(),children.end(),0.0);return true;}
    high+=volume/(childArea*geometry.size());
    if(!std::isfinite(high))return false;
    double head=low;
    for(int iteration=0;iteration<80;++iteration) {
        head=low+(high-low)*.5;double sum=0;
        for(const auto& g:geometry)sum+=g.Volume(head,childArea);
        if(!std::isfinite(sum))return false;
        if(std::abs(sum-volume)<=volume*1e-12||std::nextafter(low,high)>=high)break;
        if(sum>volume)high=head;else low=head;
    }
    double sum=0;size_t residual=0;
    for(size_t i=0;i<geometry.size();++i) {
        children[i]=geometry[i].Volume(head,childArea);sum+=children[i];
        if(children[i]>children[residual])residual=i;
    }
    // Preserve even sub-ULP water: never delete a tiny source volume merely
    // because its reconstructed head rounds to the terrain minimum.
    if(sum==0)for(size_t i=1;i<geometry.size();++i)
        if(geometry[i].Minimum()<geometry[residual].Minimum())residual=i;
    children[residual]+=volume-sum;
    return std::all_of(children.begin(),children.end(),[](double v){return std::isfinite(v)&&v>=0;});
}

}
