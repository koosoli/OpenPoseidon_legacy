using System;
using System.Drawing;

// Diagnostics only: difference pixels are not a material or puddle segmentation.
public static class TerrainPuddleMetrics
{
    public sealed class PairResult
    {
        public int Width, Height, Pixels, ChangedPixels, MaxChannel;
        public double MeanAbsChannel, ChangedFraction;
    }
    public sealed class MaskResult
    {
        public int Pixels, FirstMaskPixels, SecondMaskPixels, Intersection, Union;
        public bool NonEmpty;
        public double IoU, AgreementFraction;
    }
    public sealed class BrightnessResult
    {
        public int Pixels;
        public double MeanChannel, Above153Fraction, Above242Fraction;
    }
    private static void SameSize(Bitmap a, Bitmap b)
    {
        if (a.Width != b.Width || a.Height != b.Height)
            throw new ArgumentException("Capture dimensions differ.");
    }
    private static Rectangle Roi(Bitmap image)
    {
        if (image.Width < 5 || image.Height < 5)
            throw new ArgumentException("Capture too small for central ROI.");
        int left = image.Width / 5, top = image.Height / 5;
        return Rectangle.FromLTRB(left, top, image.Width - left, image.Height - top);
    }
    private static int MaximumDifference(Color a, Color b)
    {
        return Math.Max(Math.Abs(a.R-b.R), Math.Max(Math.Abs(a.G-b.G), Math.Abs(a.B-b.B)));
    }
    private static void ValidThreshold(int threshold)
    {
        if (threshold < 1 || threshold > 255) throw new ArgumentOutOfRangeException("threshold");
    }
    public static PairResult Compare(string first, string second, int threshold)
    {
        using (var a = new Bitmap(first))
        using (var b = new Bitmap(second)) return CompareBitmaps(a,b,threshold);
    }
    public static PairResult CompareBitmaps(Bitmap a, Bitmap b, int threshold)
    {
        ValidThreshold(threshold); SameSize(a,b);
        Rectangle roi = Roi(a);
        var result = new PairResult { Width = a.Width, Height = a.Height, Pixels = roi.Width*roi.Height };
        long sum = 0;
        for (int y=roi.Top; y<roi.Bottom; ++y)
        for (int x=roi.Left; x<roi.Right; ++x)
        {
            Color ca = a.GetPixel(x,y), cb = b.GetPixel(x,y);
            int maximum = MaximumDifference(ca,cb);
            sum += Math.Abs(ca.R-cb.R)+Math.Abs(ca.G-cb.G)+Math.Abs(ca.B-cb.B);
            result.MaxChannel = Math.Max(result.MaxChannel,maximum);
            if (maximum >= threshold) ++result.ChangedPixels;
        }
        result.MeanAbsChannel = (double)sum/(3*result.Pixels);
        result.ChangedFraction = (double)result.ChangedPixels/result.Pixels;
        return result;
    }
    public static MaskResult MaskOverlap(string dryA,string wetA,string dryB,string wetB,int threshold)
    {
        using (var da = new Bitmap(dryA))
        using (var wa = new Bitmap(wetA))
        using (var db = new Bitmap(dryB))
        using (var wb = new Bitmap(wetB)) return MaskOverlapBitmaps(da,wa,db,wb,threshold);
    }
    public static MaskResult MaskOverlapBitmaps(Bitmap da,Bitmap wa,Bitmap db,Bitmap wb,int threshold)
    {
        ValidThreshold(threshold); SameSize(da,wa); SameSize(da,db); SameSize(da,wb);
        Rectangle roi = Roi(da);
        var result = new MaskResult { Pixels = roi.Width*roi.Height };
        for (int y=roi.Top; y<roi.Bottom; ++y)
        for (int x=roi.Left; x<roi.Right; ++x)
        {
            bool a = MaximumDifference(da.GetPixel(x,y),wa.GetPixel(x,y)) >= threshold;
            bool b = MaximumDifference(db.GetPixel(x,y),wb.GetPixel(x,y)) >= threshold;
            if (a) ++result.FirstMaskPixels;
            if (b) ++result.SecondMaskPixels;
            if (a && b) ++result.Intersection;
            if (a || b) ++result.Union;
        }
        result.NonEmpty = result.Union != 0;
        // Empty masks mean no visible response, not evidence of stable puddles.
        result.IoU = result.NonEmpty ? (double)result.Intersection/result.Union : 0;
        result.AgreementFraction = 1.0-(double)(result.Union-result.Intersection)/result.Pixels;
        return result;
    }
    public static BrightnessResult Brightness(string path)
    {
        using (var bitmap = new Bitmap(path))
        {
            Rectangle roi = Roi(bitmap);
            var result = new BrightnessResult { Pixels = roi.Width*roi.Height };
            long sum = 0; int above153 = 0, above242 = 0;
            for (int y=roi.Top; y<roi.Bottom; ++y)
            for (int x=roi.Left; x<roi.Right; ++x)
            {
                Color c = bitmap.GetPixel(x,y);
                double value = (c.R+c.G+c.B)/3.0;
                sum += c.R+c.G+c.B;
                if (value >= 153) ++above153;
                if (value >= 242) ++above242;
            }
            result.MeanChannel = (double)sum/(3*result.Pixels);
            result.Above153Fraction = (double)above153/result.Pixels;
            result.Above242Fraction = (double)above242/result.Pixels;
            return result;
        }
    }
}
